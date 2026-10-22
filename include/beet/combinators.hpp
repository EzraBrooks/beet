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
#include "beet/scope_exit.hpp"
#include "beet/task.hpp"

namespace beet {
namespace detail {

template <class N>
inline constexpr bool accepts_any_input_v = std::is_same_v<input_t<N>, unit>;

template <class A, class B>
inline constexpr bool feeds_v =
    accepts_any_input_v<B> || std::constructible_from<input_t<B>, output_t<A>>;

template <class... Cs>
inline constexpr bool chains_v = []<std::size_t... I>(
                                     std::index_sequence<I...> /*unused*/) {
  using T = std::tuple<Cs...>;
  return (feeds_v<std::tuple_element_t<I, T>, std::tuple_element_t<I + 1, T>> &&
          ...);
}(std::make_index_sequence<sizeof...(Cs) - 1>{});

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
  static constexpr bool sync = (Cs::is_sync && ...);

  std::tuple<Cs...> nodes;

  template <class Trace>
  Task<Result<Out, Err>> run(In in, Trace trace) const {
    return run_all(std::move(in), trace, std::index_sequence_for<Cs...>{});
  }

  template <class Trace>
  [[nodiscard]] Result<Out, Err> run_sync(In in, Trace trace) const {
    return step<0>(std::move(in), trace);
  }

 private:
  // One frame for the whole sequence: the fold stops at the first failure,
  // which completes this task.
  template <class Trace, std::size_t... I>
  Task<Result<Out, Err>> run_all(In in, Trace trace,
                                 std::index_sequence<I...> /*unused*/) const {
    std::tuple<std::optional<In>, std::optional<output_t<Cs>>...> values;
    std::get<0>(values).emplace(std::move(in));
    ((void)std::get<I + 1>(values).emplace(co_await await_child(
         std::get<I>(nodes),
         // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
         feed<child_t<I>>(std::move(*std::get<I>(values))),
         child_trace<sequence_impl, I>(trace))),
     ...);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    co_return std::move(*std::get<sizeof...(Cs)>(values));
  }

  template <std::size_t I, class V, class Trace>
  [[nodiscard]] Result<Out, Err> step(V value, Trace trace) const {
    auto result =
        std::get<I>(nodes).run_sync(feed<child_t<I>>(std::move(value)),
                                    child_trace<sequence_impl, I>(trace));
    if (!result) {
      return make_unexpected(coerce<Err>(std::move(result).error()));
    }
    if constexpr (I + 1 == sizeof...(Cs)) {
      return std::move(*result);
    } else {
      return step<I + 1>(std::move(*result), trace);
    }
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
  using handler_traits = call_traits<Handler, E&&>;
  template <class E>
  using handler_error_t = typename handler_traits<E>::err;
  template <class E>
  static constexpr bool handler_sync_v = !handler_traits<E>::lifted::is_task;

  static constexpr std::string_view kind = "recover";
  using children = type_list<N>;
  using Out = output_t<N>;
  using Err =
      error_union_t<error_remove_t<error_t<N>, Hs...>, handler_error_t<Hs>...>;
  static constexpr bool sync = N::is_sync && (handler_sync_v<Hs> && ...);

  N inner;
  Handler handler;

  template <class Trace>
  Task<Result<Out, Err>> run(input_t<N> in, Trace trace) const {
    auto result = co_await settle_child(inner, std::move(in),
                                        child_trace<recover_impl, 0>(trace));
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

  template <class Trace>
  [[nodiscard]] Result<Out, Err> run_sync(input_t<N> in, Trace trace) const {
    auto result =
        inner.run_sync(std::move(in), child_trace<recover_impl, 0>(trace));
    if (result) {
      return std::move(*result);
    }
    if constexpr (is_variant_v<error_t<N>>) {
      return std::visit(
          [this](auto& error) { return this->handle_sync(std::move(error)); },
          result.error());
    } else {
      return handle_sync(std::move(result.error()));
    }
  }

 private:
  template <class E>
  Task<Result<Out, Err>> handle(E error) const {
    if constexpr ((std::is_same_v<E, Hs> || ...)) {
      if constexpr (!handler_sync_v<E>) {
        co_return to_result<Out, Err>(
            co_await settle(call(handler, std::move(error))));
      } else {
        co_return handle_sync(std::move(error));
      }
    } else {
      co_return handle_sync(std::move(error));
    }
  }

  template <class E>
  [[nodiscard]] Result<Out, Err> handle_sync(E&& error) const {
    static_assert(!std::is_reference_v<E>);
    if constexpr ((std::is_same_v<E, Hs> || ...)) {
      using C = handler_traits<E>;
      return to_result<Out, Err>(call_sync<typename C::out, typename C::err,
                                           may_throw_v<Handler, E&&>>(
          handler, std::forward<E>(error)));
    } else {
      return make_unexpected(coerce<Err>(std::forward<E>(error)));
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
  static constexpr bool sync = (Cs::is_sync && ...);

  std::tuple<Cs...> nodes;

  template <class Trace>
  Task<Result<Out, Err>> run(In in, Trace trace) const {
    return attempt<0>(std::move(in), trace);
  }

  template <class Trace>
  [[nodiscard]] Result<Out, Err> run_sync(In in, Trace trace) const {
    return attempt_sync<0>(in, trace);
  }

 private:
  template <std::size_t I, class Trace>
  Task<Result<Out, Err>> attempt(In in, Trace trace) const {
    if constexpr (I + 1 == sizeof...(Cs)) {
      co_return to_result<Out, Err>(co_await settle_child(
          std::get<I>(nodes), feed<child_t<I>>(std::move(in)),
          child_trace<fallback_impl, I>(trace)));
    } else {
      auto result =
          co_await settle_child(std::get<I>(nodes), feed<child_t<I>>(in),
                                child_trace<fallback_impl, I>(trace));
      if (result) {
        co_return coerce<Out>(std::move(*result));
      }
      co_return co_await attempt<I + 1>(std::move(in), trace);
    }
  }

  template <std::size_t I, class Trace>
  [[nodiscard]] Result<Out, Err> attempt_sync(In& in, Trace trace) const {
    if constexpr (I + 1 == sizeof...(Cs)) {
      return to_result<Out, Err>(
          std::get<I>(nodes).run_sync(feed<child_t<I>>(std::move(in)),
                                      child_trace<fallback_impl, I>(trace)));
    } else {
      auto result = std::get<I>(nodes).run_sync(
          feed<child_t<I>>(in), child_trace<fallback_impl, I>(trace));
      if (result) {
        return coerce<Out>(std::move(*result));
      }
      return attempt_sync<I + 1>(in, trace);
    }
  }
};

template <class N, class F>
struct finally_impl {
  static constexpr std::string_view kind = "finally";
  using children = type_list<N>;
  static constexpr bool sync = N::is_sync;

  N inner;
  F f;

  template <class Trace>
  Task<result_t<N>> run(input_t<N> in, Trace trace) const {
    scope_exit on_exit{[this] { f(); }};
    co_return co_await settle_child(inner, std::move(in),
                                    child_trace<finally_impl, 0>(trace));
  }

  template <class Trace>
  [[nodiscard]] result_t<N> run_sync(input_t<N> in, Trace trace) const {
    scope_exit on_exit{[this] { f(); }};
    return inner.run_sync(std::move(in), child_trace<finally_impl, 0>(trace));
  }
};

template <std::size_t N>
struct fixed_string {
  std::array<char, N> chars{};

  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  constexpr fixed_string(const char (&str)[N]) : chars(std::to_array(str)) {}
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
  static constexpr bool sync = N::is_sync;

  N inner;

  template <class Trace>
  Task<result_t<N>> run(input_t<N> in, Trace trace) const {
    return inner.run(std::move(in), trace);
  }

  template <class Trace>
  [[nodiscard]] result_t<N> run_sync(input_t<N> in, Trace trace) const {
    return inner.run_sync(std::move(in), trace);
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

/// Labels a node in `describe`'s output, e.g. `named<"plan">(plan_path)`. The
/// label lives only in the type.
template <detail::fixed_string Label, class N>
auto named(N child) {
  auto inner = node(std::move(child));
  using I = detail::named_impl<Label, decltype(inner)>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>,
              error_t<decltype(inner)>>(I{std::move(inner)});
}

}  // namespace beet
