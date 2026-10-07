#pragma once

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
  using Out = output_t<B>;
  using Err = error_union_t<error_t<A>, error_t<B>>;

  A a;
  B b;

  Task<Result<Out, Err>> operator()(input_t<A> in) const {
    auto value = co_await a(std::move(in));
    co_return co_await b(feed<B>(std::move(value)));
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

  using Out = output_t<N>;
  using Err = error_union_t<error_remove_t<error_t<N>, Hs...>, handler_error_t<Hs>...>;

  N inner;
  Handler handler;

  Task<Result<Out, Err>> operator()(input_t<N> in) const {
    auto r = co_await settle(inner(std::move(in)));
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
  using Out = union_t<output_t<A>, output_t<B>>;
  using Err = error_t<B>;

  A a;
  B b;

  Task<Result<Out, Err>> operator()(input_t<A> in) const {
    auto first = co_await settle(a(in));
    if (first) co_return coerce<Out>(std::move(*first));
    co_return to_result<Out, Err>(co_await settle(b(feed<B>(std::move(in)))));
  }
};

template <class N, class F>
struct finally_impl {
  struct guard {
    const F* f;
    ~guard() { (*f)(); }
  };

  N inner;
  F f;

  Task<result_t<N>> operator()(input_t<N> in) const {
    guard g{&f};
    co_return co_await settle(inner(std::move(in)));
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

}  // namespace beet
