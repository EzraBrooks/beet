#pragma once

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "beet/node.hpp"
#include "beet/task.hpp"

namespace beet {

namespace detail {
template <class Obs>
struct traced;  // defined in beet/observe.hpp

template <class Obs>
struct observer_slot {
  Obs* observer;
  std::uint64_t ticks = 0;
};
template <>
struct observer_slot<void> {};
}  // namespace detail

/// Owns a tree and its input, and drives it one tick at a time.
///
/// `Runner<N>` runs the tree untraced. `Runner<N, Obs>` (constructed with an
/// observer, see beet/observe.hpp) reports node events to `Obs`.
template <node_type N, class Obs = void>
class Runner {
 public:
  using input_type = input_t<N>;
  using result_type = result_t<N>;

  Runner(N tree, input_type input)
    requires std::is_void_v<Obs>
      : tree_(std::move(tree)), input_(std::move(input)) {}

  explicit Runner(N tree)
    requires std::is_void_v<Obs> && std::default_initializable<input_type>
      : Runner(std::move(tree), input_type{}) {}

  template <class O>
    requires std::is_same_v<O, Obs>
  Runner(N tree, input_type input, O& observer)
      : tree_(std::move(tree)), input_(std::move(input)), slot_{&observer} {}

  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  /// Starts the tree on the first call, then resumes whichever node last
  /// suspended on `running`. Once the tree has finished, returns its final
  /// status without re-running it.
  Status tick() {
    if constexpr (std::is_void_v<Obs>) {
      return advance(detail::untraced{});
    } else {
      const std::uint64_t tick = slot_.ticks++;
      slot_.observer->on_tick_begin(tick);
      const Status status = advance(detail::traced<Obs>{slot_.observer, 0});
      slot_.observer->on_tick_end(tick, status);
      return status;
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

  template <class Trace>
  Status advance(Trace trace) {
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
  [[no_unique_address]] detail::observer_slot<Obs> slot_;
  std::optional<run_state> run_;
};

template <class N, class I>
Runner(N, I) -> Runner<N>;
template <class N>
Runner(N) -> Runner<N>;
template <class N, class I, class O>
Runner(N, I, O&) -> Runner<N, O>;

}  // namespace beet
