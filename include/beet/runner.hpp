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
