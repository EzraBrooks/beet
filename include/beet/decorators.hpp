#pragma once

#include <cstddef>
#include <functional>
#include <stdexcept>
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

template <class N>
struct retry_impl {
  static constexpr std::string_view kind = "retry";
  using children = type_list<N>;

  N inner;
  std::size_t attempts;

  template <class Trace>
  Task<result_t<N>> run(input_t<N> in, Trace trace) const {
    for (std::size_t i = 1;; ++i) {
      auto r =
          co_await settle(inner.run(in, child_trace<retry_impl, 0>(trace)));
      if (r || i >= attempts) co_return std::move(r);
    }
  }
};

template <class N>
struct repeat_impl {
  static constexpr std::string_view kind = "repeat";
  using children = type_list<N>;

  N inner;
  std::size_t times;

  template <class Trace>
  Task<result_t<N>> run(input_t<N> in, Trace trace) const {
    for (std::size_t i = 1;; ++i) {
      auto value = co_await inner.run(in, child_trace<repeat_impl, 0>(trace));
      if (i >= times) co_return std::move(value);
    }
  }
};

template <class N>
struct timeout_impl {
  static constexpr std::string_view kind = "timeout_ticks";
  using children = type_list<N>;
  using Err = error_union_t<error_t<N>, Timeout>;

  N inner;
  std::size_t ticks;

  template <class Trace>
  Task<Result<output_t<N>, Err>> run(input_t<N> in, Trace trace) const {
    TaskRunner<result_t<N>> child(
        inner.run(std::move(in), child_trace<timeout_impl, 0>(trace)));
    for (std::size_t t = 1;; ++t) {
      if (child.tick() != Status::Running)
        co_return to_result<output_t<N>, Err>(std::move(child.result()));
      if (t >= ticks) co_return make_unexpected(coerce<Err>(Timeout{ticks}));
      co_await running;
    }
  }
};

inline std::size_t at_least_one(std::size_t n, const char* what) {
  if (n == 0) throw std::invalid_argument(what);
  return n;
}

}  // namespace detail

/// Re-runs `n` on the same input until it succeeds, up to `attempts` times.
/// Fails with the last error.
template <class N>
auto retry(std::size_t attempts, N n) {
  auto inner = node(std::move(n));
  using I = detail::retry_impl<decltype(inner)>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              error_t<decltype(inner)>>(
      I{std::move(inner),
        detail::at_least_one(attempts,
                             "beet::retry: attempts must be at least 1")});
}

/// Runs `n` on the same input `times` times in a row, stopping at the first
/// failure. Yields the last output.
template <class N>
auto repeat(std::size_t times, N n) {
  auto inner = node(std::move(n));
  using I = detail::repeat_impl<decltype(inner)>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              error_t<decltype(inner)>>(
      I{std::move(inner),
        detail::at_least_one(times, "beet::repeat: times must be at least 1")});
}

/// Fails with `Timeout` and halts `n` if it is still running after `ticks`
/// ticks.
template <class N>
auto timeout_ticks(std::size_t ticks, N n) {
  auto inner = node(std::move(n));
  using I = detail::timeout_impl<decltype(inner)>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              typename I::Err>(
      I{std::move(inner),
        detail::at_least_one(ticks,
                             "beet::timeout_ticks: ticks must be at least 1")});
}

/// Passes its input through if `pred(input)` holds, otherwise fails with
/// `ConditionFailed`.
template <class Pred>
auto condition(Pred pred) {
  using T = detail::callable_input_t<Pred>;
  return node<T>(
      [pred = std::move(pred)](T value) -> Result<T, ConditionFailed> {
        if (std::invoke(pred, std::as_const(value))) return value;
        return make_unexpected(ConditionFailed{});
      });
}

}  // namespace beet
