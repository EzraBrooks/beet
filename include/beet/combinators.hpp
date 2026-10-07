#pragma once

#include <cstddef>
#include <string_view>
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
struct then_impl {
  static constexpr std::string_view kind = "then";
  using children = type_list<A, B>;
  using Out = output_t<B>;
  using Err = error_union_t<error_t<A>, error_t<B>>;

  A a;
  B b;

  template <class Trace>
  Task<Result<Out, Err>> run(input_t<A> in, Trace trace) const {
    auto value = co_await a.run(std::move(in), child_trace<then_impl, 0>(trace));
    co_return co_await b.run(feed<B>(std::move(value)), child_trace<then_impl, 1>(trace));
  }
};

template <class N, class Handler, class Handled>
struct recover_impl;

template <class N, class Handler, class... Hs>
struct recover_impl<N, Handler, type_list<Hs...>> {
  static_assert((std::is_invocable_v<const Handler&, Hs&&> && ...),
                "beet: recover() handler cannot be called with every handled error type");

  template <class E>
  using handler_error_t = typename lift_of<std::invoke_result_t<const Handler&, E&&>>::err;

  static constexpr std::string_view kind = "recover";
  using children = type_list<N>;
  using Out = output_t<N>;
  using Err = error_union_t<error_remove_t<error_t<N>, Hs...>, handler_error_t<Hs>...>;

  N inner;
  Handler handler;

  template <class Trace>
  Task<Result<Out, Err>> run(input_t<N> in, Trace trace) const {
    auto r = co_await settle(inner.run(std::move(in), child_trace<recover_impl, 0>(trace)));
    if (r) co_return std::move(*r);
    if constexpr (is_variant_v<error_t<N>>) {
      co_return co_await settle(std::visit([this](auto& e) { return this->handle(std::move(e)); }, r.error()));
    } else {
      co_return co_await settle(handle(std::move(r.error())));
    }
  }

  template <class E>
  Task<Result<Out, Err>> handle(E e) const {
    if constexpr ((std::is_same_v<E, Hs> || ...)) {
      co_return to_result<Out, Err>(co_await settle(call(handler, std::move(e))));
    } else {
      co_return make_unexpected(coerce<Err>(std::move(e)));
    }
  }
};

template <class A, class B>
struct fallback_impl {
  static constexpr std::string_view kind = "fallback";
  using children = type_list<A, B>;
  using Out = union_t<output_t<A>, output_t<B>>;
  using Err = error_t<B>;

  A a;
  B b;

  template <class Trace>
  Task<Result<Out, Err>> run(input_t<A> in, Trace trace) const {
    auto first = co_await settle(a.run(in, child_trace<fallback_impl, 0>(trace)));
    if (first) co_return coerce<Out>(std::move(*first));
    co_return to_result<Out, Err>(
        co_await settle(b.run(feed<B>(std::move(in)), child_trace<fallback_impl, 1>(trace))));
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
    guard g{&f};
    co_return co_await settle(inner.run(std::move(in), child_trace<finally_impl, 0>(trace)));
  }
};

template <std::size_t N>
struct fixed_string {
  char chars[N]{};

  constexpr fixed_string(const char (&s)[N]) {
    for (std::size_t i = 0; i < N; ++i) chars[i] = s[i];
  }
  constexpr std::string_view view() const { return {chars, N - 1}; }
};

/// Attaches a label to a node. Transparent: it adds no node, no ID, and no runtime state.
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

template <class Impl, class In, class Out, class Err>
template <class Next>
auto Node<Impl, In, Out, Err>::then(Next next) const {
  auto b = node(std::move(next));
  using B = decltype(b);
  static_assert(detail::accepts_any_input_v<B> || std::constructible_from<input_t<B>, Out>,
                "beet: then() target cannot accept this node's output type");
  using I = detail::then_impl<Node, B>;
  return Node<I, In, typename I::Out, typename I::Err>(I{*this, std::move(b)});
}

template <class Impl, class In, class Out, class Err>
template <class... Es, class Handler>
auto Node<Impl, In, Out, Err>::recover(Handler handler) const {
  static_assert((error_contains_v<Es, Err> && ...), "beet: recover<E>() names an error type this node cannot produce");
  using Handled = std::conditional_t<sizeof...(Es) == 0, set_list_t<Err>, type_list<Es...>>;
  using I = detail::recover_impl<Node, Handler, Handled>;
  return Node<I, In, Out, typename I::Err>(I{*this, std::move(handler)});
}

template <class Impl, class In, class Out, class Err>
template <class Alt>
auto Node<Impl, In, Out, Err>::fallback(Alt alt) const {
  auto b = node(std::move(alt));
  using B = decltype(b);
  static_assert(detail::accepts_any_input_v<B> || std::constructible_from<input_t<B>, const In&>,
                "beet: fallback() alternative cannot accept this node's input type");
  using I = detail::fallback_impl<Node, B>;
  return Node<I, In, typename I::Out, typename I::Err>(I{*this, std::move(b)});
}

template <class Impl, class In, class Out, class Err>
template <class F>
auto Node<Impl, In, Out, Err>::finally(F f) const {
  static_assert(std::is_invocable_v<const F&>, "beet: finally() takes a callable with no arguments");
  using I = detail::finally_impl<Node, F>;
  return Node<I, In, Out, Err>(I{*this, std::move(f)});
}

/// `sequence(a, b, c)` is `node(a).then(b).then(c)`.
template <class First, class... Rest>
auto sequence(First first, Rest... rest) {
  if constexpr (sizeof...(Rest) == 0) {
    return node(std::move(first));
  } else {
    return node(std::move(first)).then(sequence(std::move(rest)...));
  }
}

/// `fallback(a, b, c)` tries each alternative in order on the same input until one succeeds.
template <class First, class... Rest>
auto fallback(First first, Rest... rest) {
  if constexpr (sizeof...(Rest) == 0) {
    return node(std::move(first));
  } else {
    return node(std::move(first)).fallback(fallback(std::move(rest)...));
  }
}

template <class A, class B>
  requires(node_type<A> || node_type<B>)
auto operator|(A a, B b) {
  return node(std::move(a)).then(std::move(b));
}

/// Labels a node for observers, e.g. `named<"plan">(plan_path)`. The label lives only in the type.
template <detail::fixed_string Label, class N>
auto named(N n) {
  auto inner = node(std::move(n));
  using I = detail::named_impl<Label, decltype(inner)>;
  return Node<I, input_t<decltype(inner)>, output_t<decltype(inner)>, error_t<decltype(inner)>>(I{std::move(inner)});
}

}  // namespace beet
