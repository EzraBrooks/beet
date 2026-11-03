#pragma once

#include <atomic>
#include <exception>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#include "beet/meta.hpp"
#include "beet/node.hpp"
#include "beet/result.hpp"
#include "beet/task.hpp"

namespace beet {

/// `offload` strategy for jobs that read nothing the tick also touches: the
/// node's input goes to the job unchanged.
struct passthrough_t {};
inline constexpr passthrough_t passthrough{};

namespace detail {

template <class Args>
struct offload_input : input_from_args<Args> {};
template <>
struct offload_input<type_list<std::stop_token>> {
  using type = unit;
};
template <class A>
struct offload_input<type_list<A, std::stop_token>> {
  using type = std::decay_t<A>;
};

// Node input: the job's own input for `passthrough`, otherwise `prepare`'s.
template <class P, class F>
struct offload_in {
  using type = callable_input_t<P>;
};
template <class F>
struct offload_in<passthrough_t, F>
    : offload_input<typename callable_traits<F>::args> {};

template <class P, class In>
auto run_prepare(const P& prepare, In&& in) {
  if constexpr (std::is_same_v<P, passthrough_t>) {
    return std::forward<In>(in);
  } else if constexpr (std::is_invocable_v<const P&, In>) {
    return prepare(std::forward<In>(in));
  } else {
    return prepare();
  }
}

template <class F, class In>
decltype(auto) invoke_job(const F& fn, In&& in, const std::stop_token& stop) {
  if constexpr (std::is_invocable_v<const F&, In, std::stop_token>) {
    return fn(std::forward<In>(in), stop);
  } else if constexpr (std::is_invocable_v<const F&, In>) {
    return fn(std::forward<In>(in));
  } else if constexpr (std::is_invocable_v<const F&, std::stop_token>) {
    return fn(stop);
  } else {
    return fn();
  }
}

}  // namespace detail

/// Runs `job` on its own thread so slow work does not stall the tick; the node
/// stays running until `job` returns. `prepare` runs first, on the tick, and
/// `job` receives only its result, so state the tick shares can be copied
/// there instead of read from the job's thread. Pass `beet::passthrough` when
/// the job needs nothing shared. `job` may take a trailing `std::stop_token`,
/// which is stopped when the node is halted. Halting waits for `job` to
/// return, so long-running jobs should poll the token.
template <class P, class F>
auto offload(P prepare, F job) {
  using In = typename detail::offload_in<P, F>::type;
  using S = decltype(detail::run_prepare(prepare, std::declval<In>()));
  static_assert(!std::is_void_v<S> && !std::is_pointer_v<S>,
                "beet::offload prepare must return a value for the job, not "
                "void or a pointer");
  using R =
      decltype(detail::invoke_job(job, std::declval<S>(), std::stop_token{}));
  using L = detail::lift_of<R>;
  static_assert(!L::is_task,
                "beet::offload runs plain functions; coroutines already yield "
                "to the tick");
  using Out = typename L::out;
  using Err = typename L::err;

  return node<In>([prepare = std::move(prepare),
                   job = std::move(job)](In in) -> Task<Result<Out, Err>> {
    S snapshot = detail::run_prepare(prepare, std::move(in));
    std::atomic<bool> done{false};
    std::optional<Result<Out, Err>> value;
    std::exception_ptr exception;
    // Declared last: destroying the frame stops and joins the worker before
    // anything it refers to goes away.
    std::jthread worker([&](const std::stop_token& stop) {
      auto work = [&] {
        if constexpr (std::is_void_v<R>) {
          detail::invoke_job(job, std::move(snapshot), stop);
          value.emplace(Out{});
        } else {
          value.emplace(detail::to_result<Out, Err>(
              detail::invoke_job(job, std::move(snapshot), stop)));
        }
      };
#if BEET_EXCEPTIONS
      try {
        work();
      } catch (...) {
        exception = std::current_exception();
      }
#else
      work();
#endif
      done.store(true, std::memory_order_release);
    });

    while (!done.load(std::memory_order_acquire)) {
      co_await running;
    }
    if (exception) {
      std::rethrow_exception(exception);
    }
    co_return std::move(*value);
  });
}

}  // namespace beet
