#pragma once

#include <cstddef>
#include <functional>
#include <string_view>
#include <utility>

#include "beet/meta.hpp"
#include "beet/node.hpp"
#include "beet/result.hpp"
#include "beet/task.hpp"

namespace beet {

struct Timeout {
  std::size_t ticks;
  friend bool operator==(const Timeout&, const Timeout&) = default;
};

struct ConditionFailed {
  friend bool operator==(const ConditionFailed&,
                         const ConditionFailed&) = default;
};

namespace detail {

template <class N, std::size_t Attempts>
struct retry_impl {
  static constexpr std::string_view kind = "retry";
  using children = type_list<N>;
  static constexpr bool sync = N::is_sync;

  N inner;

  template <class Trace>
  Task<result_t<N>> run(input_t<N> in, Trace trace) const {
    for (std::size_t i = 1;; ++i) {
      auto result =
          co_await settle_child(inner, in, child_trace<retry_impl, 0>(trace));
      if (result || i >= Attempts) {
        co_return std::move(result);
      }
    }
  }

  template <class Trace>
  [[nodiscard]] result_t<N> run_sync(input_t<N> in, Trace trace) const {
    for (std::size_t i = 1;; ++i) {
      auto result = inner.run_sync(in, child_trace<retry_impl, 0>(trace));
      if (result || i >= Attempts) {
        return result;
      }
    }
  }
};

template <class N, std::size_t Times>
struct repeat_impl {
  static constexpr std::string_view kind = "repeat";
  using children = type_list<N>;
  static constexpr bool sync = N::is_sync;

  N inner;

  template <class Trace>
  Task<result_t<N>> run(input_t<N> in, Trace trace) const {
    for (std::size_t i = 1;; ++i) {
      auto value =
          co_await await_child(inner, in, child_trace<repeat_impl, 0>(trace));
      if (i >= Times) {
        co_return std::move(value);
      }
    }
  }

  template <class Trace>
  [[nodiscard]] result_t<N> run_sync(input_t<N> in, Trace trace) const {
    for (std::size_t i = 1;; ++i) {
      auto result = inner.run_sync(in, child_trace<repeat_impl, 0>(trace));
      if (!result || i >= Times) {
        return result;
      }
    }
  }
};

template <class N, std::size_t Ticks>
struct timeout_impl {
  static constexpr std::string_view kind = "timeout_ticks";
  using children = type_list<N>;
  using Err = error_union_t<error_t<N>, Timeout>;
  // A sync child finishes on its first tick, which is always within the limit.
  static constexpr bool sync = N::is_sync;

  N inner;

  template <class Trace>
  Task<Result<output_t<N>, Err>> run(input_t<N> in, Trace trace) const {
    TaskRunner<result_t<N>> child(
        inner.run(std::move(in), child_trace<timeout_impl, 0>(trace)));
    for (std::size_t tick = 1;; ++tick) {
      if (child.tick() != Status::Running) {
        co_return to_result<output_t<N>, Err>(std::move(child.result()));
      }
      if (tick >= Ticks) {
        co_return make_unexpected(coerce<Err>(Timeout{Ticks}));
      }
      co_await running;
    }
  }

  template <class Trace>
  [[nodiscard]] Result<output_t<N>, Err> run_sync(input_t<N> in,
                                                  Trace trace) const {
    return to_result<output_t<N>, Err>(
        inner.run_sync(std::move(in), child_trace<timeout_impl, 0>(trace)));
  }
};

}  // namespace detail

/// Re-runs `child` on the same input until it succeeds, up to `Attempts` times.
/// Fails with the last error.
template <std::size_t Attempts, class N>
auto retry(N child) {
  static_assert(Attempts >= 1, "beet: retry<Attempts>() needs Attempts >= 1");
  auto inner = node(std::move(child));
  using I = detail::retry_impl<decltype(inner), Attempts>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              error_t<decltype(inner)>>(I{std::move(inner)});
}

/// Runs `child` on the same input `Times` times in a row, stopping at the first
/// failure. Yields the last output.
template <std::size_t Times, class N>
auto repeat(N child) {
  static_assert(Times >= 1, "beet: repeat<Times>() needs Times >= 1");
  auto inner = node(std::move(child));
  using I = detail::repeat_impl<decltype(inner), Times>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              error_t<decltype(inner)>>(I{std::move(inner)});
}

/// Fails with `Timeout` and halts `child` if it is still running after `Ticks`
/// ticks.
template <std::size_t Ticks, class N>
auto timeout_ticks(N child) {
  static_assert(Ticks >= 1, "beet: timeout_ticks<Ticks>() needs Ticks >= 1");
  auto inner = node(std::move(child));
  using I = detail::timeout_impl<decltype(inner), Ticks>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              typename I::Err>(I{std::move(inner)});
}

/// Passes its input through if `pred(input)` holds, otherwise fails with
/// `ConditionFailed`.
template <class Pred>
auto condition(Pred pred) {
  using T = detail::callable_input_t<Pred>;
  return node<T>(
      [pred = std::move(pred)](T value) noexcept(
          noexcept(static_cast<bool>(std::invoke(std::declval<const Pred&>(),
                                                 std::declval<const T&>()))) &&
              std::is_nothrow_move_constructible_v<T>)
          -> Result<T, ConditionFailed> {
        if (std::invoke(pred, std::as_const(value))) {
          return value;
        }
        return make_unexpected(ConditionFailed{});
      });
}

}  // namespace beet
