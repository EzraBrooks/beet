#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <string_view>
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
  static constexpr std::string_view kind = "leaf";

  F f;

  template <class Trace>
  auto run(In in, Trace) const {
    if constexpr (std::is_invocable_v<const F&, In>) {
      return call(f, std::move(in));
    } else {
      return call(f);
    }
  }
};

/// Trace used when no observer is attached. Every tracing hook on this path compiles to nothing.
struct untraced {
  template <std::size_t Offset>
  constexpr untraced child() const noexcept {
    return {};
  }
};

template <class Impl>
inline constexpr bool is_transparent_v = requires { Impl::transparent; };

template <class Impl>
struct impl_children {
  using type = type_list<>;
};
template <class Impl>
  requires requires { typename Impl::children; }
struct impl_children<Impl> {
  using type = typename Impl::children;
};

template <class N>
struct subtree_size;

/// Number of nodes in `Impl`'s subtree, counting itself. Transparent impls (labels) add none.
template <class Impl, class Children = typename impl_children<Impl>::type>
struct impl_subtree_size;
template <class Impl, class... Cs>
struct impl_subtree_size<Impl, type_list<Cs...>> {
  static constexpr std::size_t value = [] {
    if constexpr (is_transparent_v<Impl>) {
      return subtree_size<typename Impl::inner_type>::value;
    } else {
      return (std::size_t{1} + ... + subtree_size<Cs>::value);
    }
  }();
};

template <class Impl, class In, class Out, class Err>
struct subtree_size<Node<Impl, In, Out, Err>> : std::integral_constant<std::size_t, impl_subtree_size<Impl>::value> {};

template <class N>
inline constexpr std::size_t subtree_size_v = subtree_size<std::remove_cvref_t<N>>::value;

/// Depth-first ID of child `I` relative to its parent.
template <class Impl, std::size_t I, class Children = typename impl_children<Impl>::type>
struct child_offset;
template <class Impl, std::size_t I, class... Cs>
struct child_offset<Impl, I, type_list<Cs...>> {
  static constexpr std::size_t value = [] {
    constexpr std::size_t sizes[] = {subtree_size<Cs>::value..., 0};
    std::size_t offset = 1;
    for (std::size_t j = 0; j < I; ++j) offset += sizes[j];
    return offset;
  }();
};

template <class Impl, std::size_t I, class Trace>
constexpr auto child_trace(const Trace& trace) {
  return trace.template child<child_offset<Impl, I>::value>();
}

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
  using impl_type = Impl;

  explicit Node(Impl impl) : impl_(std::move(impl)) {}

  /// Type-erases another node into an `AnyNode`, widening its error set if needed.
  template <node_type Other>
    requires(detail::is_std_function<Impl>::value && !std::is_same_v<std::remove_cvref_t<Other>, Node>)
  Node(Other other) : impl_(detail::erase<In, Out, Err>(std::move(other))) {}

  Task<result_type> operator()(In in) const { return run(std::move(in), detail::untraced{}); }

  /// Runs the node under `trace`. Only traces other than `untraced` report events to an observer.
  template <class Trace>
  Task<result_type> run(In in, Trace trace) const {
    if constexpr (std::is_same_v<Trace, detail::untraced> || detail::is_transparent_v<Impl>) {
      return run_impl(std::move(in), trace);
    } else {
      return trace.wrap(run_impl(std::move(in), trace));
    }
  }

 private:
  template <class Trace>
  Task<result_type> run_impl(In in, Trace trace) const {
    if constexpr (detail::is_std_function<Impl>::value) {
      return impl_(std::move(in));
    } else {
      return impl_.run(std::move(in), trace);
    }
  }

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
    using L = detail::lift_of<decltype(std::declval<const Impl&>().run(std::declval<In>(), detail::untraced{}))>;
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
