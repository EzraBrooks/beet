#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include "beet/meta.hpp"
#include "beet/result.hpp"
#include "beet/task.hpp"

namespace beet {

template <class Impl, class In, class Out, class Err>
class Node;

namespace detail {
template <class T>
struct is_node : std::false_type {};
template <class Impl, class In, class Out, class Err>
struct is_node<Node<Impl, In, Out, Err>> : std::true_type {};
}  // namespace detail

template <class T>
concept node_type = detail::is_node<std::remove_cvref_t<T>>::value;

template <node_type N>
using input_t = typename std::remove_cvref_t<N>::input_type;
template <node_type N>
using output_t = typename std::remove_cvref_t<N>::output_type;
template <node_type N>
using error_t = typename std::remove_cvref_t<N>::error_type;
template <node_type N>
using result_t = typename std::remove_cvref_t<N>::result_type;

namespace detail {

template <class T>
struct is_std_function : std::false_type {};
template <class Sig>
struct is_std_function<std::function<Sig>> : std::true_type {};

/// Maps a callable's return type to the node output and error types it produces.
template <class R>
struct lift_traits {
  using out = R;
  using err = never;
  static constexpr bool is_task = false;
};
template <>
struct lift_traits<void> {
  using out = unit;
  using err = never;
  static constexpr bool is_task = false;
};
template <class T, class E>
struct lift_traits<Result<T, E>> {
  using out = T;
  using err = E;
  static constexpr bool is_task = false;
};
template <class E>
struct lift_traits<Result<void, E>> {
  using out = unit;
  using err = E;
  static constexpr bool is_task = false;
};
template <class T, class E>
struct lift_traits<Task<Result<T, E>>> {
  static_assert(!std::is_void_v<T>, "beet: coroutine nodes must return Task<Result<T, E>> with non-void T");
  using out = T;
  using err = E;
  static constexpr bool is_task = true;
};

template <class R>
using lift_of = lift_traits<std::remove_cvref_t<R>>;

/// Converts any callable result (`Out`, `void`, or a `Result`) into `Result<Out, Err>`.
template <class Out, class Err, class R>
Result<Out, Err> to_result(R&& r) {
  using V = std::remove_cvref_t<R>;
  if constexpr (is_result_v<V>) {
    if (!r.has_value()) return make_unexpected(coerce<Err>(std::forward<R>(r).error()));
    if constexpr (std::is_void_v<typename V::value_type>) {
      return Out{};
    } else {
      return coerce<Out>(*std::forward<R>(r));
    }
  } else {
    return coerce<Out>(std::forward<R>(r));
  }
}

template <class Out, class Err, class F, class... Args>
Task<Result<Out, Err>> lazy_call(F f, Args... args) {
  if constexpr (std::is_void_v<std::invoke_result_t<const F&, Args&&...>>) {
    std::invoke(std::as_const(f), std::move(args)...);
    co_return Result<Out, Err>(Out{});
  } else {
    co_return to_result<Out, Err>(std::invoke(std::as_const(f), std::move(args)...));
  }
}

/// Invokes `f` as a node body. Plain callables are deferred into a task so they run when first ticked.
template <class F, class... Args>
auto call(const F& f, Args&&... args) {
  using L = lift_of<std::invoke_result_t<const F&, Args&&...>>;
  if constexpr (L::is_task) {
    return std::invoke(f, std::forward<Args>(args)...);
  } else {
    return lazy_call<typename L::out, typename L::err>(f, std::decay_t<Args>(std::forward<Args>(args))...);
  }
}

/// Builds the input for node `N`, discarding the value when `N` takes no input.
template <class N, class V>
input_t<N> feed(V&& value) {
  if constexpr (std::is_same_v<input_t<N>, unit>) {
    return unit{};
  } else {
    return input_t<N>(std::forward<V>(value));
  }
}

template <class Out, class Err, class R>
Task<Result<Out, Err>> adapt(Task<R> task) {
  co_return to_result<Out, Err>(co_await settle(std::move(task)));
}

template <class In, class Out, class Err, class N>
std::function<Task<Result<Out, Err>>(In)> erase(N n) {
  static_assert(std::is_same_v<input_t<N>, unit> || std::constructible_from<input_t<N>, In>,
                "beet: AnyNode input is not convertible to the wrapped node's input");
  static_assert(error_subset_v<error_t<N>, Err>, "beet: AnyNode error set does not cover the wrapped node's errors");
  if constexpr (std::is_same_v<input_t<N>, In> && std::is_same_v<result_t<N>, Result<Out, Err>>) {
    return [n = std::move(n)](In in) { return n(std::move(in)); };
  } else {
    return [n = std::move(n)](In in) { return adapt<Out, Err>(n(feed<N>(std::move(in)))); };
  }
}

template <class F, class In>
struct lift_impl {
  F f;

  auto operator()(In in) const {
    if constexpr (std::is_invocable_v<const F&, In>) {
      return call(f, std::move(in));
    } else {
      return call(f);
    }
  }
};

}  // namespace detail

/// A typed behavior tree node: `In -> Task<Result<Out, Err>>`.
///
/// A node and its `Runner` must outlive any task created from it, since combinator tasks refer
/// back to their children.
template <class Impl, class In, class Out, class Err>
class Node {
 public:
  using input_type = In;
  using output_type = Out;
  using error_type = Err;
  using result_type = Result<Out, Err>;

  explicit Node(Impl impl) : impl_(std::move(impl)) {}

  /// Type-erases another node into an `AnyNode`, widening its error set if needed.
  template <node_type Other>
    requires(detail::is_std_function<Impl>::value && !std::is_same_v<std::remove_cvref_t<Other>, Node>)
  Node(Other other) : impl_(detail::erase<In, Out, Err>(std::move(other))) {}

  Task<result_type> operator()(In in) const { return impl_(std::move(in)); }

  /// Runs `next` on this node's output. Error sets are combined.
  template <class Next>
  auto then(Next next) const;

  /// Handles the listed error types (all of them if none are listed) and removes them from the error set.
  /// The handler may return `Out`, `Result<Out, E2>`, or `Task<Result<Out, E2>>`; `E2` joins the error set.
  template <class... Es, class Handler>
  auto recover(Handler handler) const;

  /// Runs `alt` on the same input if this node fails. The error is `alt`'s.
  template <class Alt>
  auto fallback(Alt alt) const;

  /// Calls `f()` when this node finishes, fails, or is halted.
  template <class F>
  auto finally(F f) const;

 private:
  Impl impl_;
};

template <class In, class Out, class Err>
using AnyNode = Node<std::function<Task<Result<Out, Err>>(In)>, In, Out, Err>;

/// Lifts a callable into a node with an explicit input type. Use this for generic lambdas.
template <class In, class F>
auto node(F f) {
  if constexpr (node_type<F>) {
    static_assert(std::is_same_v<input_t<F>, In>, "beet: node<In>() given a node with a different input type");
    return f;
  } else {
    using Impl = detail::lift_impl<F, In>;
    using L = detail::lift_of<decltype(std::declval<const Impl&>()(std::declval<In>()))>;
    return Node<Impl, In, typename L::out, typename L::err>(Impl{std::move(f)});
  }
}

/// Lifts a callable `Out(In)`, `Result<Out, E>(In)`, or `Task<Result<Out, E>>(In)` into a node.
/// Callables taking no argument get `unit` input. Nodes are returned unchanged.
template <class F>
auto node(F f) {
  if constexpr (node_type<F>) {
    return f;
  } else {
    return node<detail::callable_input_t<F>>(std::move(f));
  }
}

}  // namespace beet

#include "beet/combinators.hpp"  // IWYU pragma: export
