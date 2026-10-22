#pragma once

#include <utility>

namespace beet::detail {

/// Calls `fn` when the guard goes out of scope, unless `release()`d. Modeled on
/// `std::experimental::scope_exit`.
template <class F>
class scope_exit {
 public:
  explicit scope_exit(F fn) : fn_(std::move(fn)) {}
  scope_exit(const scope_exit&) = delete;
  scope_exit& operator=(const scope_exit&) = delete;
  ~scope_exit() {
    if (active_) {
      fn_();
    }
  }

  void release() noexcept { active_ = false; }

 private:
  F fn_;
  bool active_ = true;
};

}  // namespace beet::detail
