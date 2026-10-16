#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "beet/meta.hpp"
#include "beet/node.hpp"
#include "beet/result.hpp"
#include "beet/task.hpp"

namespace beet {
namespace detail {

template <class N>
inline constexpr bool accepts_any_input_v = std::is_same_v<input_t<N>, unit>;

template <class A, class B>
inline constexpr bool feeds_v =
    accepts_any_input_v<B> || std::constructible_from<input_t<B>, output_t<A>>;

template <class Seq, class... Cs>
struct chains;
template <std::size_t... I, class... Cs>
struct chains<std::index_sequence<I...>, Cs...> {
  using T = std::tuple<Cs...>;
  static constexpr bool value =
      (feeds_v<std::tuple_element_t<I, T>, std::tuple_element_t<I + 1, T>> &&
       ...);
};
template <class... Cs>
inline constexpr bool chains_v =
    chains<std::make_index_sequence<sizeof...(Cs) - 1>, Cs...>::value;

/// Runs its children in order, feeding each one's output to the next.
template <class... Cs>
struct sequence_impl {
  static constexpr std::string_view kind = "sequence";
  using children = type_list<Cs...>;
  template <std::size_t I>
  using child_t = std::tuple_element_t<I, std::tuple<Cs...>>;
  using In = input_t<child_t<0>>;
  using Out = output_t<child_t<sizeof...(Cs) - 1>>;
  using Err = error_union_t<error_t<Cs>...>;

  std::tuple<Cs...> nodes;

  template <class Trace>
  Task<Result<Out, Err>> run(In in, Trace trace) const {
    return run_all(std::move(in), trace, std::index_sequence_for<Cs...>{});
  }

 private:
  // One frame for the whole sequence: the fold stops at the first failure,
  // which completes this task.
  template <class Trace, std::size_t... I>
  Task<Result<Out, Err>> run_all(In in, Trace trace,
                                 std::index_sequence<I...> /*unused*/) const {
    std::tuple<std::optional<In>, std::optional<output_t<Cs>>...> values;
    std::get<0>(values).emplace(std::move(in));
    ((void)std::get<I + 1>(values).emplace(co_await std::get<I>(nodes).run(
         feed<child_t<I>>(std::move(*std::get<I>(values))),
         child_trace<sequence_impl, I>(trace))),
     ...);
    co_return std::move(*std::get<sizeof...(Cs)>(values));
  }
};

template <class N, class Handler, class Handled>
struct recover_impl;

template <class N, class Handler, class... Hs>
struct recover_impl<N, Handler, type_list<Hs...>> {
  static_assert(
      (std::is_invocable_v<const Handler&, Hs&&> && ...),
      "beet: recover() handler cannot be called with every handled error type");

  template <class E>
  using handler_error_t = typename call_traits<Handler, E&&>::err;

  static constexpr std::string_view kind = "recover";
  using children = type_list<N>;
  using Out = output_t<N>;
  using Err =
      error_union_t<error_remove_t<error_t<N>, Hs...>, handler_error_t<Hs>...>;

  N inner;
  Handler handler;

  template <class Trace>
  Task<Result<Out, Err>> run(input_t<N> in, Trace trace) const {
    auto result = co_await settle(
        inner.run(std::move(in), child_trace<recover_impl, 0>(trace)));
    if (result) {
      co_return std::move(*result);
    }
    if constexpr (is_variant_v<error_t<N>>) {
      co_return co_await settle(std::visit(
          [this](auto& error) { return this->handle(std::move(error)); },
          result.error()));
    } else {
      co_return co_await settle(handle(std::move(result.error())));
    }
  }

  template <class E>
  Task<Result<Out, Err>> handle(E error) const {
    if constexpr ((std::is_same_v<E, Hs> || ...)) {
      co_return to_result<Out, Err>(
          co_await settle(call(handler, std::move(error))));
    } else {
      co_return make_unexpected(coerce<Err>(std::move(error)));
    }
  }
};

/// Tries its children in order on the same input until one succeeds.
template <class... Cs>
struct fallback_impl {
  static constexpr std::string_view kind = "fallback";
  using children = type_list<Cs...>;
  template <std::size_t I>
  using child_t = std::tuple_element_t<I, std::tuple<Cs...>>;
  using In = input_t<child_t<0>>;
  using Out = union_t<output_t<Cs>...>;
  using Err = error_t<child_t<sizeof...(Cs) - 1>>;

  std::tuple<Cs...> nodes;

  template <class Trace>
  Task<Result<Out, Err>> run(In in, Trace trace) const {
    return attempt<0>(std::move(in), trace);
  }

 private:
  template <std::size_t I, class Trace>
  Task<Result<Out, Err>> attempt(In in, Trace trace) const {
    if constexpr (I + 1 == sizeof...(Cs)) {
      co_return to_result<Out, Err>(co_await settle(
          std::get<I>(nodes).run(feed<child_t<I>>(std::move(in)),
                                 child_trace<fallback_impl, I>(trace))));
    } else {
      auto result = co_await settle(std::get<I>(nodes).run(
          feed<child_t<I>>(in), child_trace<fallback_impl, I>(trace)));
      if (result) {
        co_return coerce<Out>(std::move(*result));
      }
      co_return co_await attempt<I + 1>(std::move(in), trace);
    }
  }
};

template <class N, class F>
struct finally_impl {
  static constexpr std::string_view kind = "finally";
  using children = type_list<N>;

  struct guard {
    const F* f;
    ~guard() { (*f)(); }
  };

  N inner;
  F f;

  template <class Trace>
  Task<result_t<N>> run(input_t<N> in, Trace trace) const {
    guard on_exit{&f};
    co_return co_await settle(
        inner.run(std::move(in), child_trace<finally_impl, 0>(trace)));
  }
};

template <std::size_t N>
struct fixed_string {
  std::array<char, N> chars{};

  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  constexpr fixed_string(const char (&str)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
      chars[i] = str[i];
    }
  }
  [[nodiscard]] constexpr std::string_view view() const {
    return {chars.data(), N - 1};
  }
};

/// Attaches a label to a node. Transparent: it adds no node, no ID, and no
/// runtime state.
template <fixed_string Label, class N>
struct named_impl {
  static constexpr bool transparent = true;
  static constexpr std::string_view label = Label.view();
  using inner_type = N;

  N inner;

  template <class Trace>
  Task<result_t<N>> run(input_t<N> in, Trace trace) const {
    return inner.run(std::move(in), trace);
  }
};

}  // namespace detail

namespace detail {

template <class... Cs>
auto make_sequence(Cs... children) {
  static_assert(
      chains_v<Cs...>,
      "beet: sequence() child cannot accept the previous child's output type");
  using I = sequence_impl<Cs...>;
  return Node<I, typename I::In, typename I::Out, typename I::Err>(
      I{{std::move(children)...}});
}

template <class First, class... Rest>
auto make_fallback(First first, Rest... rest) {
  static_assert(
      ((accepts_any_input_v<Rest> ||
        std::constructible_from<input_t<Rest>, const input_t<First>&>) &&
       ...),
      "beet: fallback() alternative cannot accept the first child's input "
      "type");
  using I = fallback_impl<First, Rest...>;
  return Node<I, typename I::In, typename I::Out, typename I::Err>(
      I{{std::move(first), std::move(rest)...}});
}

}  // namespace detail

/// Runs its children in order, each on the previous child's output, and fails
/// at the first failure. The error set is the union of the children's.
template <class First, class... Rest>
auto sequence(First first, Rest... rest) {
  return detail::make_sequence(node(std::move(first)),
                               node(std::move(rest))...);
}

/// Tries its children in order on the same input until one succeeds. The error
/// is the last child's.
template <class First, class... Rest>
auto fallback(First first, Rest... rest) {
  return detail::make_fallback(node(std::move(first)),
                               node(std::move(rest))...);
}

/// Handles the listed error types of `child` (all of them if none are listed)
/// and removes them from the error set. The handler may return `Out`,
/// `Result<Out, E2>`, or `Task<Result<Out, E2>>`; `E2` joins the error set.
template <class... Es, class N, class Handler>
auto recover(N child, Handler handler) {
  auto inner = node(std::move(child));
  using C = decltype(inner);
  static_assert(
      (error_contains_v<Es, error_t<C>> && ...),
      "beet: recover<E>() names an error type this node cannot produce");
  using Handled = std::conditional_t<sizeof...(Es) == 0, set_list_t<error_t<C>>,
                                     type_list<Es...>>;
  using I = detail::recover_impl<C, Handler, Handled>;
  return Node<I, input_t<C>, output_t<C>, typename I::Err>(
      I{std::move(inner), std::move(handler)});
}

/// Calls `fn()` when `child` finishes, fails, or is halted.
template <class N, class F>
auto finally(N child, F fn) {
  static_assert(std::is_invocable_v<const F&>,
                "beet: finally() takes a callable with no arguments");
  auto inner = node(std::move(child));
  using C = decltype(inner);
  using I = detail::finally_impl<C, F>;
  return Node<I, input_t<C>, output_t<C>, error_t<C>>(
      I{std::move(inner), std::move(fn)});
}

/// Labels a node for observers, e.g. `named<"plan">(plan_path)`. The label
/// lives only in the type.
template <detail::fixed_string Label, class N>
auto named(N child) {
  auto inner = node(std::move(child));
  using I = detail::named_impl<Label, decltype(inner)>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              error_t<decltype(inner)>>(I{std::move(inner)});
}

}  // namespace beet
