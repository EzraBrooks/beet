#pragma once

#include <concepts>
#include <optional>
#include <stdexcept>
#include <utility>

#include "beet/node.hpp"
#include "beet/task.hpp"

namespace beet {

/// Owns a tree and its input, and drives it one tick at a time.
template <node_type N>
class Runner {
 public:
  using input_type = input_t<N>;
  using result_type = result_t<N>;

  Runner(N tree, input_type input) : tree_(std::move(tree)), input_(std::move(input)) {}
  explicit Runner(N tree)
    requires std::default_initializable<input_type>
      : Runner(std::move(tree), input_type{}) {}

  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  /// Starts the tree on the first call, then resumes whichever node last suspended on `running`.
  /// Once the tree has finished, returns its final status without re-running it.
  Status tick() {
    if (!run_) run_.emplace(tree_(input_));
    return run_->tick();
  }

  Status status() const { return run_ ? run_->status() : Status::Idle; }

  /// Halts every running node. The next `tick()` starts the tree again from scratch.
  void halt() { run_.reset(); }

  void reset(input_type input) {
    halt();
    input_ = std::move(input);
  }

  const result_type& result() const {
    if (!run_ || !run_->done()) throw std::logic_error("beet::Runner::result() called before the tree finished");
    return run_->result();
  }

 private:
  N tree_;
  input_type input_;
  std::optional<detail::TaskRunner<result_type>> run_;
};

template <class N, class I>
Runner(N, I) -> Runner<N>;
template <class N>
Runner(N) -> Runner<N>;

}  // namespace beet
