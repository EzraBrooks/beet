#pragma once

#include <atomic>
#include <exception>
#include <memory>
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

template <class F, class In>
decltype(auto) invoke_job(const F& f, In&& in, std::stop_token stop) {
  if constexpr (std::is_invocable_v<const F&, In, std::stop_token>) {
    return f(std::forward<In>(in), std::move(stop));
  } else if constexpr (std::is_invocable_v<const F&, In>) {
    return f(std::forward<In>(in));
  } else if constexpr (std::is_invocable_v<const F&, std::stop_token>) {
    return f(std::move(stop));
  } else {
    return f();
  }
}

template <class R>
struct offload_state {
  std::stop_source stop;
  std::atomic<bool> done{false};
  std::optional<R> value;
  std::exception_ptr exception;
};

}  // namespace detail

/// Runs `fn` on its own thread so slow work does not stall the tick; the node
/// stays running until `fn` returns. `fn` may take a trailing
/// `std::stop_token`, which is stopped when the node is halted. A halted job
/// still runs to completion on its thread, and its result is dropped.
template <class F>
auto offload(F fn) {
  using In = typename detail::offload_input<
      typename detail::callable_traits<F>::args>::type;
  using R =
      decltype(detail::invoke_job(fn, std::declval<In>(), std::stop_token{}));
  using L = detail::lift_of<R>;
  static_assert(!L::is_task,
                "beet::offload runs plain functions; coroutines already yield "
                "to the tick");
  using Out = typename L::out;
  using Err = typename L::err;
  using State = detail::offload_state<Result<Out, Err>>;

  return node<In>([fn = std::move(fn)](In in) -> Task<Result<Out, Err>> {
    auto state = std::make_shared<State>();
    std::thread([state, fn, in = std::move(in)]() mutable {
      auto job = [&] {
        if constexpr (std::is_void_v<R>) {
          detail::invoke_job(fn, std::move(in), state->stop.get_token());
          state->value.emplace(Out{});
        } else {
          state->value.emplace(detail::to_result<Out, Err>(
              detail::invoke_job(fn, std::move(in), state->stop.get_token())));
        }
      };
#if BEET_EXCEPTIONS
      try {
        job();
      } catch (...) {
        state->exception = std::current_exception();
      }
#else
      job();
#endif
      state->done.store(true, std::memory_order_release);
    }).detach();

    struct stop_on_exit {
      std::stop_source stop;
      ~stop_on_exit() { stop.request_stop(); }
    } guard{state->stop};

    while (!state->done.load(std::memory_order_acquire)) co_await running;
    if (state->exception) std::rethrow_exception(state->exception);
    co_return std::move(*state->value);
  });
}

}  // namespace beet
