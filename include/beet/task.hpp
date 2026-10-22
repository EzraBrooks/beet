#pragma once

// `Task<T>` is a lazily started coroutine that resumes its children by
// symmetric transfer. `detail::task_promise<T>` is its coroutine
// `promise_type`; it is unrelated to `std::promise`.

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

#include "beet/meta.hpp"
#include "beet/result.hpp"

namespace beet {

enum class Status : std::uint8_t { Idle, Running, Success, Failure };

struct running_t {
  explicit running_t() = default;
};

/// `co_await beet::running;` suspends the node until the next tick.
inline constexpr running_t running{};

template <class T>
class Task;

template <class T>
struct settle_t {
  Task<T> task;
};

/// Awaits a child and returns its whole `Result` instead of propagating its
/// error.
template <class T>
settle_t<T> settle(Task<T>&& task) {
  return {std::move(task)};
}

namespace detail {

/// An already computed child result, awaited like a finished `Task<R>` without
/// a coroutine frame. `Settled` returns the whole result instead of
/// propagating its error.
template <class R, bool Settled = false>
struct ready_result {
  R result;
};

}  // namespace detail

template <class R>
detail::ready_result<R, true> settle(detail::ready_result<R>&& ready) {
  return {std::move(ready.result)};
}

namespace detail {

/// Per-runner state shared by every coroutine frame in one running tree.
struct tick_context {
  std::coroutine_handle<> leaf;  // innermost frame suspended on `running`
};

template <class T>
struct task_promise;

struct task_promise_base {
  tick_context* ctx = nullptr;
  std::coroutine_handle<> continuation;
  std::exception_ptr exception;

  // Set when the awaiting parent propagates this task's error instead of
  // resuming.
  task_promise_base* propagate_to = nullptr;

  void unhandled_exception() noexcept { exception = std::current_exception(); }
};

struct final_awaiter {
  [[nodiscard]] bool await_ready() const noexcept { return false; }
  template <class P>
  std::coroutine_handle<> await_suspend(
      std::coroutine_handle<P> handle) noexcept {
    return handle.promise().on_final();
  }
  void await_resume() const noexcept {}
};

struct running_awaiter {
  task_promise_base* self;

  [[nodiscard]] bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> caller) const noexcept {
    assert(self->ctx && "beet::running awaited outside of a Runner");
    self->ctx->leaf = caller;
  }
  void await_resume() const noexcept {}
};

template <class U>
struct settle_awaiter {
  Task<U> task;
  task_promise_base* parent;

  [[nodiscard]] bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(
      std::coroutine_handle<> caller) noexcept {
    auto& child = task.handle().promise();
    child.ctx = parent->ctx;
    child.continuation = caller;
    return task.handle();
  }
  U await_resume() {
    auto& child = task.handle().promise();
    if (child.exception) {
      std::rethrow_exception(child.exception);
    }
    assert(child.value && "beet: child task finished without a value");
    return std::move(*child.value);
  }
};

/// Awaits a child `Result`, yielding its value; a failure completes the parent
/// with the error.
template <class U, class T>
struct try_awaiter {
  static_assert(error_subset_v<typename U::error_type, typename T::error_type>,
                "beet: co_await on a child whose errors are not covered by "
                "this coroutine's error set");

  Task<U> task;
  task_promise_base* parent;

  static std::coroutine_handle<> propagate(task_promise<U>& child) noexcept {
    if (!child.value || child.value->has_value()) {
      return {};
    }
    auto& outer = static_cast<task_promise<T>&>(*child.propagate_to);
    outer.value.emplace(unexpect, coerce<typename T::error_type>(
                                      std::move(child.value->error())));
    return outer.on_final();
  }

  [[nodiscard]] bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(
      std::coroutine_handle<> caller) noexcept {
    auto& child = task.handle().promise();
    child.ctx = parent->ctx;
    child.continuation = caller;
    child.propagate_to = parent;
    child.propagate = &propagate;
    return task.handle();
  }
  typename U::value_type await_resume() {
    auto& child = task.handle().promise();
    if (child.exception) {
      std::rethrow_exception(child.exception);
    }
    assert(child.value && "beet: child task finished without a value");
    return std::move(**child.value);
  }
};

template <class U>
struct ready_settle_awaiter {
  U result;

  [[nodiscard]] bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<> /*unused*/) const noexcept {}
  U await_resume() { return std::move(result); }
};

/// Like `try_awaiter` for a result that is already known: a failure completes
/// the parent with the error without resuming it.
template <class U, class T>
struct ready_try_awaiter {
  static_assert(error_subset_v<typename U::error_type, typename T::error_type>,
                "beet: co_await on a child whose errors are not covered by "
                "this coroutine's error set");

  U result;
  task_promise_base* parent;

  [[nodiscard]] bool await_ready() const noexcept { return result.has_value(); }
  std::coroutine_handle<> await_suspend(
      std::coroutine_handle<> /*unused*/) noexcept {
    auto& outer = static_cast<task_promise<T>&>(*parent);
    outer.value.emplace(
        unexpect, coerce<typename T::error_type>(std::move(result.error())));
    return outer.on_final();
  }
  typename U::value_type await_resume() { return std::move(*result); }
};

template <class T>
struct task_promise final : task_promise_base {
  // Installed by a `try_awaiter`, which knows the parent's result type that
  // this promise cannot name.
  using propagate_fn = std::coroutine_handle<> (*)(task_promise&) noexcept;

  std::optional<T> value;
  propagate_fn propagate = nullptr;

  std::coroutine_handle<> on_final() noexcept {
    if (propagate != nullptr) {
      if (auto next = propagate(*this)) {
        return next;
      }
    }
    return continuation ? continuation : std::noop_coroutine();
  }

  Task<T> get_return_object() noexcept;
  [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
    return {};
  }
  [[nodiscard]] final_awaiter final_suspend() const noexcept { return {}; }

  template <class V = T>
    requires std::constructible_from<T, V&&>
  void return_value(V&& result) {
    value.emplace(std::forward<V>(result));
  }

  running_awaiter await_transform(running_t /*unused*/) noexcept {
    return {this};
  }

  template <class U>
  settle_awaiter<U> await_transform(settle_t<U>&& settled) noexcept {
    return {std::move(settled.task), this};
  }

  template <class U>
  auto await_transform(Task<U>&& task) noexcept {
    if constexpr (is_result_v<T> && is_result_v<U>) {
      return try_awaiter<U, T>{std::move(task), this};
    } else {
      return settle_awaiter<U>{std::move(task), this};
    }
  }

  template <class U, bool Settled>
  auto await_transform(ready_result<U, Settled>&& ready) {
    if constexpr (!Settled && is_result_v<T> && is_result_v<U>) {
      return ready_try_awaiter<U, T>{std::move(ready.result), this};
    } else {
      return ready_settle_awaiter<U>{std::move(ready.result)};
    }
  }

  [[nodiscard]] bool done() const noexcept {
    return value.has_value() || exception;
  }
};

}  // namespace detail

/// Lazily started coroutine. Destroying a suspended `Task` halts it and every
/// child it is awaiting.
template <class T>
class [[nodiscard]] Task {
 public:
  using promise_type = detail::task_promise<T>;
  using value_type = T;

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  [[nodiscard]] std::coroutine_handle<promise_type> handle() const noexcept {
    return handle_;
  }

 private:
  friend promise_type;
  explicit Task(std::coroutine_handle<promise_type> handle) noexcept
      : handle_(handle) {}

  std::coroutine_handle<promise_type> handle_;
};

template <class T>
Task<T> detail::task_promise<T>::get_return_object() noexcept {
  return Task<T>(std::coroutine_handle<task_promise<T>>::from_promise(*this));
}

namespace detail {

struct runner_base {
  virtual ~runner_base() = default;
  virtual Status tick() = 0;
};

/// Drives one task tick by tick with its own `tick_context`.
template <class T>
class TaskRunner final : public runner_base {
 public:
  explicit TaskRunner(Task<T> task) : task_(std::move(task)) {}
  TaskRunner(const TaskRunner&) = delete;
  TaskRunner& operator=(const TaskRunner&) = delete;

  Status tick() override {
    auto& state = task_.handle().promise();
    if (!state.done()) {
      if (!started_) {
        started_ = true;
        state.ctx = &ctx_;
        task_.handle().resume();
      } else {
        assert(ctx_.leaf && "beet: unfinished task has no suspended leaf");
        std::exchange(ctx_.leaf, {}).resume();
      }
    }
    if (state.exception) {
      std::rethrow_exception(state.exception);
    }
    return status();
  }

  [[nodiscard]] Status status() const {
    const auto& state = task_.handle().promise();
    if (!state.value) {
      return started_ ? Status::Running : Status::Idle;
    }
    if constexpr (is_result_v<T>) {
      return state.value->has_value() ? Status::Success : Status::Failure;
    } else {
      return Status::Success;
    }
  }

  [[nodiscard]] bool done() const { return task_.handle().promise().done(); }
  T& result() {
    auto& value = task_.handle().promise().value;
    assert(value &&
           "beet: TaskRunner::result() called before the task finished");
    return *value;
  }
  [[nodiscard]] const T& result() const {
    const auto& value = task_.handle().promise().value;
    assert(value &&
           "beet: TaskRunner::result() called before the task finished");
    return *value;
  }

 private:
  Task<T> task_;
  tick_context ctx_;
  bool started_ = false;
};

}  // namespace detail
}  // namespace beet
