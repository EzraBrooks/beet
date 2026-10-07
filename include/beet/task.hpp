#pragma once

#include <cassert>
#include <concepts>
#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include "beet/meta.hpp"
#include "beet/result.hpp"

namespace beet {

enum class Status { Idle, Running, Success, Failure };

/// Per-runner state shared by every coroutine frame in one running tree.
struct TickContext {
  std::coroutine_handle<> leaf;  // innermost frame suspended on `running`
};

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

/// Awaits a child and returns its whole `Result` instead of propagating its error.
template <class T>
settle_t<T> settle(Task<T>&& task) {
  return {std::move(task)};
}

namespace detail {

template <class T>
struct promise;

struct promise_base {
  TickContext* ctx = nullptr;
  std::coroutine_handle<> continuation;
  std::exception_ptr exception;

  // Set when the awaiting parent propagates this task's error instead of resuming.
  promise_base* propagate_to = nullptr;
  std::coroutine_handle<> (*propagate)(promise_base& self) noexcept = nullptr;

  std::coroutine_handle<> on_final() noexcept {
    if (propagate) {
      if (auto next = propagate(*this)) return next;
    }
    return continuation ? continuation : std::noop_coroutine();
  }

  void unhandled_exception() noexcept { exception = std::current_exception(); }
};

struct final_awaiter {
  bool await_ready() const noexcept { return false; }
  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
    return h.promise().on_final();
  }
  void await_resume() const noexcept {}
};

struct running_awaiter {
  promise_base* self;

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) const noexcept {
    assert(self->ctx && "beet::running awaited outside of a Runner");
    self->ctx->leaf = h;
  }
  void await_resume() const noexcept {}
};

template <class U>
struct raw_awaiter {
  Task<U> task;
  promise_base* parent;

  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
    auto& child = task.handle().promise();
    child.ctx = parent->ctx;
    child.continuation = h;
    return task.handle();
  }
  U await_resume() {
    auto& child = task.handle().promise();
    if (child.exception) std::rethrow_exception(child.exception);
    return std::move(*child.value);
  }
};

/// Awaits a child `Result`, yielding its value; a failure completes the parent with the error.
template <class U, class T>
struct try_awaiter {
  static_assert(error_subset_v<typename U::error_type, typename T::error_type>,
                "beet: co_await on a child whose errors are not covered by this coroutine's error set");

  Task<U> task;
  promise_base* parent;

  static std::coroutine_handle<> propagate(promise_base& self) noexcept {
    auto& child = static_cast<promise<U>&>(self);
    if (!child.value || child.value->has_value()) return {};
    auto& outer = static_cast<promise<T>&>(*child.propagate_to);
    outer.value.emplace(unexpect, coerce<typename T::error_type>(std::move(child.value->error())));
    return outer.on_final();
  }

  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
    auto& child = task.handle().promise();
    child.ctx = parent->ctx;
    child.continuation = h;
    child.propagate_to = parent;
    child.propagate = &propagate;
    return task.handle();
  }
  typename U::value_type await_resume() {
    auto& child = task.handle().promise();
    if (child.exception) std::rethrow_exception(child.exception);
    return std::move(**child.value);
  }
};

template <class T>
struct promise final : promise_base {
  std::optional<T> value;

  Task<T> get_return_object() noexcept;
  std::suspend_always initial_suspend() const noexcept { return {}; }
  final_awaiter final_suspend() const noexcept { return {}; }

  template <class V = T>
    requires std::constructible_from<T, V&&>
  void return_value(V&& v) {
    value.emplace(std::forward<V>(v));
  }

  running_awaiter await_transform(running_t) noexcept { return {this}; }

  template <class U>
  raw_awaiter<U> await_transform(settle_t<U>&& s) noexcept {
    return {std::move(s.task), this};
  }

  template <class U>
  auto await_transform(Task<U>&& task) noexcept {
    if constexpr (is_result_v<T> && is_result_v<U>) {
      return try_awaiter<U, T>{std::move(task), this};
    } else {
      return raw_awaiter<U>{std::move(task), this};
    }
  }

  bool done() const noexcept { return value.has_value() || exception; }
};

}  // namespace detail

/// Lazily started coroutine. Destroying a suspended `Task` halts it and every child it is awaiting.
template <class T>
class [[nodiscard]] Task {
 public:
  using promise_type = detail::promise<T>;
  using value_type = T;

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() {
    if (handle_) handle_.destroy();
  }

  std::coroutine_handle<promise_type> handle() const noexcept { return handle_; }

 private:
  friend promise_type;
  explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

  std::coroutine_handle<promise_type> handle_;
};

template <class T>
Task<T> detail::promise<T>::get_return_object() noexcept {
  return Task<T>(std::coroutine_handle<promise<T>>::from_promise(*this));
}

namespace detail {

struct runner_base {
  virtual ~runner_base() = default;
  virtual Status tick() = 0;
};

/// Drives one task tick by tick with its own `TickContext`.
template <class T>
class TaskRunner final : public runner_base {
 public:
  explicit TaskRunner(Task<T> task) : task_(std::move(task)) {}
  TaskRunner(const TaskRunner&) = delete;
  TaskRunner& operator=(const TaskRunner&) = delete;

  Status tick() override {
    auto& p = task_.handle().promise();
    if (!p.done()) {
      if (!started_) {
        started_ = true;
        p.ctx = &ctx_;
        task_.handle().resume();
      } else {
        assert(ctx_.leaf && "beet: unfinished task has no suspended leaf");
        std::exchange(ctx_.leaf, {}).resume();
      }
    }
    if (p.exception) std::rethrow_exception(p.exception);
    return status();
  }

  Status status() const {
    const auto& p = task_.handle().promise();
    if (!p.value) return started_ ? Status::Running : Status::Idle;
    if constexpr (is_result_v<T>) {
      return p.value->has_value() ? Status::Success : Status::Failure;
    } else {
      return Status::Success;
    }
  }

  bool done() const { return task_.handle().promise().done(); }
  T& result() { return *task_.handle().promise().value; }
  const T& result() const { return *task_.handle().promise().value; }

 private:
  Task<T> task_;
  TickContext ctx_;
  bool started_ = false;
};

}  // namespace detail
}  // namespace beet
