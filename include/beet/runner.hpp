#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>  // IWYU pragma: keep
#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#include "beet/node.hpp"
#include "beet/task.hpp"

namespace beet {

namespace detail {
template <class Hooks>
struct traced;  // defined in beet/observe.hpp

template <class Hooks>
struct listener_slot {
  Hooks listeners;
  std::uint64_t ticks = 0;
};
template <>
struct listener_slot<void> {};

class steady_pacer {
 public:
  [[nodiscard]] static std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
  }
  void sleep_until(std::chrono::steady_clock::time_point deadline,
                   const std::stop_token& stop) {
    if (!stop.stop_possible()) {
      std::this_thread::sleep_until(deadline);
      return;
    }
    std::unique_lock lock(mutex_);
    wake_.wait_until(lock, stop, deadline, [] { return false; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable_any wake_;
};
}  // namespace detail

/// What `Runner::run()` does after a tick finishes past the next tick's
/// deadline. Matches Tokio's `MissedTickBehavior`; in every case the late tick
/// runs right away.
enum class MissedTick : std::uint8_t {
  /// Keeps the original schedule, ticking back to back until caught up.
  Burst,
  /// Restarts the schedule from the late tick.
  Delay,
  /// Drops the deadlines that already passed and keeps the original schedule.
  Skip,
};

/// A tick that finished after the next tick's deadline.
struct Overrun {
  std::uint64_t tick;                 // the tick that ran long
  std::chrono::nanoseconds late;      // how far past the next deadline it ended
  std::uint64_t missed;               // further deadlines that also passed
  std::chrono::nanoseconds interval;  // nominal time between ticks
};

/// Writes overruns to `std::clog`, at most one line per second.
class OverrunWarning {
 public:
  void operator()(const Overrun& overrun) {
    using namespace std::chrono;
    using millis = duration<double, std::milli>;
    const auto now = steady_clock::now();
    ++pending_.ticks;
    pending_.missed += overrun.missed;
    pending_.worst = std::max(pending_.worst, overrun.late);
    if (last_warned_ && now - *last_warned_ < seconds{1}) {
      return;
    }
    std::clog << std::format(
        "beet: {} tick(s) overran the {:.3f}ms tick interval (worst +{:.3f}ms, "
        "{} further deadline(s) missed)\n",
        pending_.ticks, millis(overrun.interval).count(),
        millis(pending_.worst).count(), pending_.missed);
    last_warned_ = now;
    pending_ = {};
  }

 private:
  struct Pending {
    std::uint64_t ticks = 0;
    std::uint64_t missed = 0;
    std::chrono::nanoseconds worst{};
  };

  std::optional<std::chrono::steady_clock::time_point> last_warned_;
  Pending pending_;
};

struct RunOptions {
  MissedTick missed_tick = MissedTick::Skip;
  // Lets designated initializers omit `stop` under
  // -Wmissing-designated-field-initializers.
  std::stop_token stop{};  // NOLINT(readability-redundant-member-init)
  std::function<void(const Overrun&)> on_overrun = OverrunWarning{};
};

namespace detail {

/// Where `Runner::run()` is in its fixed-rate schedule. Slot `n` is due
/// `n - anchor_slot` intervals after `anchor`.
template <class TimePoint>
struct schedule {
  std::chrono::nanoseconds period;
  std::int64_t ticks_per_period;
  TimePoint anchor;
  std::uint64_t anchor_slot;
  std::uint64_t slot;          // the next tick's slot
  std::uint64_t behind_until;  // while bursting, the last slot known missed

  // Exact for any period, so the schedule never accumulates rounding error.
  [[nodiscard]] constexpr TimePoint deadline(std::uint64_t at) const {
    const auto since = static_cast<std::int64_t>(at - anchor_slot);
    return anchor + (since / ticks_per_period) * period +
           (since % ticks_per_period) * period / ticks_per_period;
  }
};

template <class TimePoint>
struct schedule_step {
  schedule<TimePoint> next;
  std::optional<Overrun> overrun;  // a new overrun to report
  bool late;                       // the next tick is already due
};

/// The schedule after tick number `ticked` ends at `now`.
template <class TimePoint>
constexpr schedule_step<TimePoint> next_tick(schedule<TimePoint> sched,
                                             std::uint64_t ticked,
                                             TimePoint now, MissedTick policy) {
  ++sched.slot;
  const TimePoint due = sched.deadline(sched.slot);
  if (now <= due) {
    return {.next = sched, .overrun = std::nullopt, .late = false};
  }
  // While bursting, deadlines up to `behind_until` are already known to be
  // missed, so only deadlines past them count as a new overrun.
  const std::uint64_t known = std::max(sched.slot, sched.behind_until);
  std::uint64_t last_due = known;
  while (sched.deadline(last_due + 1) <= now) {
    ++last_due;
  }
  std::optional<Overrun> overrun;
  if (last_due > sched.behind_until) {
    overrun = Overrun{.tick = ticked,
                      .late = now - due,
                      .missed = last_due - known,
                      .interval = sched.period / sched.ticks_per_period};
  }
  switch (policy) {
    case MissedTick::Burst: sched.behind_until = last_due; break;
    case MissedTick::Delay:
      sched.anchor = now;
      sched.anchor_slot = sched.slot;
      break;
    case MissedTick::Skip: sched.slot = last_due; break;
  }
  return {.next = sched, .overrun = overrun, .late = true};
}

}  // namespace detail

/// Owns a tree and its input, and drives it one tick at a time.
///
/// `Runner<N>` runs the tree untraced. `Runner<N, Hooks>` (constructed with a
/// `beet::Listeners`, see beet/observe.hpp) reports node events to its hooks.
template <node_type N, class Hooks = void>
class Runner {
 public:
  using input_type = input_t<N>;
  using result_type = result_t<N>;
  /// Ticks per period the tree needs (see `rate<K>()`), or 0 if it has no
  /// `rate<K>()` region.
  static constexpr std::size_t rate = N::rate;

  Runner(N tree, input_type input)
    requires std::is_void_v<Hooks>
      : tree_(std::move(tree)), input_(std::move(input)) {}

  explicit Runner(N tree)
    requires std::is_void_v<Hooks> && std::default_initializable<input_type>
      : Runner(std::move(tree), input_type{}) {}

  template <class L>
    requires std::same_as<L, Hooks>
  Runner(N tree, input_type input, L listeners)
      : tree_(std::move(tree)),
        input_(std::move(input)),
        slot_{std::move(listeners)} {}

  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  /// Starts the tree on the first call, then resumes whichever node last
  /// suspended on `running`. Once the tree has finished, returns its final
  /// status without re-running it.
  Status tick() {
    if constexpr (std::is_void_v<Hooks>) {
      return advance(detail::untraced{});
    } else {
      const std::uint64_t tick = slot_.ticks++;
      slot_.listeners.on_tick_begin(tick);
      const Status status = advance(detail::traced<Hooks>{&slot_.listeners, 0});
      slot_.listeners.on_tick_end(tick, status);
      return status;
    }
  }

  /// Ticks the tree `rate` times per `period` until it finishes, returning its
  /// final status, or until `options.stop` is requested, returning `Running`.
  Status run(std::chrono::nanoseconds period = std::chrono::seconds{1},
             const RunOptions& options = {}) {
    detail::steady_pacer pacer;
    return run(period, options, pacer);
  }

  /// `run()` on the clock of `pacer`, which provides `now()` and
  /// `sleep_until(time_point, std::stop_token)`.
  template <class Pacer>
  Status run(std::chrono::nanoseconds period, const RunOptions& options,
             Pacer& pacer) {
    static_assert(rate != 0,
                  "beet: Runner::run() needs a tree with a rate; wrap the root "
                  "in beet::rate<K>()");
    detail::schedule<decltype(pacer.now())> sched{
        .period = period,
        .ticks_per_period = static_cast<std::int64_t>(rate),
        .anchor = pacer.now(),
        .anchor_slot = 0,
        .slot = 0,
        .behind_until = 0};
    for (std::uint64_t ticked = 0;; ++ticked) {
      const Status status = tick();
      if (status != Status::Running) {
        return status;
      }
      if (options.stop.stop_requested()) {
        return Status::Running;
      }
      const auto step =
          detail::next_tick(sched, ticked, pacer.now(), options.missed_tick);
      sched = step.next;
      if (step.overrun && options.on_overrun) {
        options.on_overrun(*step.overrun);
      }
      if (step.late) {
        continue;
      }
      pacer.sleep_until(sched.deadline(sched.slot), options.stop);
      if (options.stop.stop_requested()) {
        return Status::Running;
      }
    }
  }

  [[nodiscard]] Status status() const {
    if (!run_) {
      return Status::Idle;
    }
    if constexpr (N::is_sync) {
      return run_->has_value() ? Status::Success : Status::Failure;
    } else {
      return run_->status();
    }
  }

  /// Halts every running node. The next `tick()` starts the tree again from
  /// scratch.
  void halt() { run_.reset(); }

  void reset(input_type input) {
    halt();
    input_ = std::move(input);
  }

  [[nodiscard]] const result_type& result() const {
    if constexpr (N::is_sync) {
      if (run_) {
        return *run_;
      }
    } else {
      if (run_ && run_->done()) {
        return run_->result();
      }
    }
#if BEET_EXCEPTIONS
    throw std::logic_error(
        "beet::Runner::result() called before the tree finished");
#else
    std::fputs("beet::Runner::result() called before the tree finished\n",
               stderr);
    std::abort();
#endif
  }

 private:
  // A sync tree finishes within one tick, so it needs no task to resume.
  using run_state = std::conditional_t<N::is_sync, result_type,
                                       detail::TaskRunner<result_type>>;

  template <class Root>
  Status advance(Root root) {
    const auto trace = [&] {
      if constexpr (rate != 0) {
        return detail::with_rate<rate>(root);
      } else {
        return root;
      }
    }();
    if constexpr (N::is_sync) {
      if (!run_) {
        run_.emplace(tree_.run_sync(input_, trace));
      }
      return status();
    } else {
      if (!run_) {
        run_.emplace(tree_.run(input_, trace));
      }
      return run_->tick();
    }
  }

  N tree_;
  input_type input_;
  [[no_unique_address]] detail::listener_slot<Hooks> slot_;
  std::optional<run_state> run_;
};

template <class N, class I>
Runner(N, I) -> Runner<N>;
template <class N>
Runner(N) -> Runner<N>;
template <class N, class I, class L>
Runner(N, I, L) -> Runner<N, L>;

}  // namespace beet
