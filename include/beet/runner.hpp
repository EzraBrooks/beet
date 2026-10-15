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

struct rate_scan {
  unsigned hz = 0;
  bool conflict = false;

  friend constexpr rate_scan operator+(rate_scan a, rate_scan b) {
    if (a.hz == 0) return b;
    if (b.hz == 0) return a;
    return {a.hz, a.conflict || b.conflict || a.hz != b.hz};
  }
};

/// The rate of the outermost `rate<Hz>` regions in a subtree, and whether
/// they disagree.
template <class N>
struct outer_rate;
template <class Impl, class In, class Out, class Err>
struct outer_rate<Node<Impl, In, Out, Err>> {
  static constexpr rate_scan value = [] {
    if constexpr (requires { Impl::hz; }) {
      return rate_scan{Impl::hz};
    } else if constexpr (is_transparent_v<Impl>) {
      return outer_rate<typename Impl::inner_type>::value;
    } else {
      return []<class... Cs>(type_list<Cs...>) {
        return (rate_scan{} + ... + outer_rate<Cs>::value);
      }(typename impl_children<Impl>::type{});
    }
  }();
};
}  // namespace detail

/// Owns a tree and its input, and drives it one tick at a time.
///
/// `Runner<N>` runs the tree untraced. `Runner<N, Obs>` (constructed with an
/// observer, see beet/observe.hpp) reports node events to `Obs`.
template <node_type N, class Obs = void>
class Runner {
  static_assert(!detail::outer_rate<N>::value.conflict,
                "beet: the outermost rate<Hz> regions disagree on the tick "
                "rate");

 public:
  using input_type = input_t<N>;
  using result_type = result_t<N>;

  /// How often `tick()` must be called, from the outermost `rate<Hz>`; 0 if
  /// the tree declares no rate.
  static constexpr unsigned tick_hz = detail::outer_rate<N>::value.hz;

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
      if (!run_) run_.emplace(tree_(input_));
      return run_->tick();
    } else {
      const std::uint64_t tick = slot_.ticks++;
      slot_.observer->on_tick_begin(tick);
      if (!run_)
        run_.emplace(tree_.run(input_, detail::traced<Obs>{slot_.observer, 0}));
      const Status status = run_->tick();
      slot_.observer->on_tick_end(tick, status);
      return status;
    }
  }

  Status status() const { return run_ ? run_->status() : Status::Idle; }

  /// Halts every running node. The next `tick()` starts the tree again from
  /// scratch.
  void halt() { run_.reset(); }

  void reset(input_type input) {
    halt();
    input_ = std::move(input);
  }

  const result_type& result() const {
    if (!run_ || !run_->done()) {
#if BEET_EXCEPTIONS
      throw std::logic_error(
          "beet::Runner::result() called before the tree finished");
#else
      std::fputs("beet::Runner::result() called before the tree finished\n",
                 stderr);
      std::abort();
#endif
    }
    return run_->result();
  }

 private:
  N tree_;
  input_type input_;
  [[no_unique_address]] detail::observer_slot<Obs> slot_;
  std::optional<detail::TaskRunner<result_type>> run_;
};

template <class N, class I>
Runner(N, I) -> Runner<N>;
template <class N>
Runner(N) -> Runner<N>;
template <class N, class I, class O>
Runner(N, I, O&) -> Runner<N, O>;

}  // namespace beet
