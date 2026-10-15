#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
      co_await running;
    }
  }
};

template <class N, bool Forever>
struct repeat_impl {
  static constexpr std::string_view kind = "repeat";
  using children = type_list<N>;
  using Out = std::conditional_t<Forever, never, output_t<N>>;

  N inner;
  std::size_t times;

  template <class Trace>
  Task<Result<Out, error_t<N>>> run(input_t<N> in, Trace trace) const {
    for (std::size_t i = 1;; ++i) {
      [[maybe_unused]] auto value =
          co_await inner.run(in, child_trace<repeat_impl, 0>(trace));
      if constexpr (!Forever) {
        if (i >= times) co_return std::move(value);
      }
      co_await running;
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

template <unsigned Hz, class N>
struct rate_impl {
  static_assert(Hz > 0, "beet: rate<Hz> needs Hz > 0");
  static constexpr std::string_view kind = "rate";
  static constexpr unsigned hz = Hz;
  using children = type_list<N>;

  N inner;

  template <class Trace>
  Task<result_t<N>> run(input_t<N> in, Trace trace) const {
    constexpr unsigned parent = trace_hz_v<Trace>;
    static_assert(parent == 0 || Hz <= parent,
                  "beet: rate<Hz> is faster than its enclosing rate");
    static_assert(parent == 0 || parent % Hz == 0,
                  "beet: rate<Hz> must evenly divide its enclosing rate");
    constexpr std::uint64_t divisor = parent == 0 ? 1 : parent / Hz;

    using Base = typename strip_rate<Trace>::type;
    const Base base = child_trace<rate_impl, 0>(trace);
    TaskRunner<result_t<N>> child(
        inner.run(std::move(in), rated<Base, Hz>{base}));
    for (std::uint64_t t = 0;; ++t) {
      if (t % divisor == 0 && child.tick() != Status::Running)
        co_return std::move(child.result());
      co_await running;
    }
  }
};

inline std::size_t at_least_one(std::size_t n, const char* what) {
#if BEET_EXCEPTIONS
  if (n == 0) throw std::invalid_argument(what);
#else
  if (n == 0) {
    std::fputs(what, stderr);
    std::abort();
  }
#endif
  return n;
}

}  // namespace detail

/// Runs `n` on the same input until it succeeds, up to `attempts` times,
/// waiting a tick between attempts. Fails with the last error.
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

/// Runs `n` on the same input `times` times, waiting a tick between runs and
/// stopping at the first failure. Yields the last output.
template <class N>
auto repeat(std::size_t times, N n) {
  auto inner = node(std::move(n));
  using I = detail::repeat_impl<decltype(inner), false>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              error_t<decltype(inner)>>(
      I{std::move(inner),
        detail::at_least_one(times, "beet::repeat: times must be at least 1")});
}

/// Runs `n` on the same input once per tick, forever, until it fails.
template <class N>
auto repeat(N n) {
  auto inner = node(std::move(n));
  using I = detail::repeat_impl<decltype(inner), true>;
  return Node<I, input_t<decltype(inner)>, never, error_t<decltype(inner)>>(
      I{std::move(inner), 0});
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

/// Ticks `n` at `Hz`: every `P / Hz`th tick of the enclosing `rate<P>`
/// region, or every tick if there is none. The outermost rate declares how
/// often its parent is ticked. Fails to compile unless `Hz` divides `P`.
template <unsigned Hz, class N>
auto rate(N n) {
  auto inner = node(std::move(n));
  using I = detail::rate_impl<Hz, decltype(inner)>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              error_t<decltype(inner)>>(I{std::move(inner)});
}

/// Passes its input through if `pred(input)` holds, otherwise fails with
/// `ConditionFailed`.
template <class Pred>
auto condition(Pred pred) {
  using T = detail::callable_input_t<Pred>;
  return node<T>([pred = std::move(pred)](T value) noexcept(
                     std::is_nothrow_invocable_v<const Pred&, const T&>&&
                         std::is_nothrow_move_constructible_v<T>)
                     -> Result<T, ConditionFailed> {
    if (std::invoke(pred, std::as_const(value))) return value;
    return make_unexpected(ConditionFailed{});
  });
}

}  // namespace beet
