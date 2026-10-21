#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <utility>

#include "beet/meta.hpp"
#include "beet/result.hpp"
#include "beet/task.hpp"

namespace beet {

template <class Impl, class In, class Out, class Err>
class Node;

template <class T>
concept node_type = detail::specialization_of<std::remove_cvref_t<T>, Node>;

template <node_type N>
using input_t = typename std::remove_cvref_t<N>::input_type;
template <node_type N>
using output_t = typename std::remove_cvref_t<N>::output_type;
template <node_type N>
using error_t = typename std::remove_cvref_t<N>::error_type;
template <node_type N>
using result_t = typename std::remove_cvref_t<N>::result_type;
/// Ticks per period the subtree needs, or 0 if it contains no `rate<K>()`.
template <node_type N>
inline constexpr std::size_t rate_v = std::remove_cvref_t<N>::rate;

namespace detail {

/// Maps a callable's return type to the node output and error types it
/// produces.
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
  static_assert(
      !std::is_void_v<T>,
      "beet: coroutine nodes must return Task<Result<T, E>> with non-void T");
  using out = T;
  using err = E;
  static constexpr bool is_task = true;
};

template <class R>
using lift_of = lift_traits<std::remove_cvref_t<R>>;

/// Converts any callable result (`Out`, `void`, or a `Result`) into
/// `Result<Out, Err>`.
template <class Out, class Err, class R>
Result<Out, Err> to_result(R&& result) {
  using V = std::remove_cvref_t<R>;
  if constexpr (is_result_v<V>) {
    if (!result.has_value()) {
      return make_unexpected(coerce<Err>(std::forward<R>(result).error()));
    }
    if constexpr (std::is_void_v<typename V::value_type>) {
      return Out{};
    } else {
      return coerce<Out>(*std::forward<R>(result));
    }
  } else {
    return coerce<Out>(std::forward<R>(result));
  }
}

/// Whether beet must catch exceptions from `f(args...)`.
template <class F, class... Args>
inline constexpr bool may_throw_v =
    BEET_EXCEPTIONS && !std::is_nothrow_invocable_v<const F&, Args...>;

/// Output and error types of `call(f, args...)`. Callables that may throw
/// gain `Exception`.
template <class F, class... Args>
struct call_traits {
  using lifted = lift_of<std::invoke_result_t<const F&, Args...>>;
  using out = typename lifted::out;
  using err = std::conditional_t<may_throw_v<F, Args...>,
                                 error_union_t<typename lifted::err, Exception>,
                                 typename lifted::err>;
};

template <class F, class... Args>
decltype(auto) invoke_value(const F& fn, Args&&... args) {
  if constexpr (std::is_void_v<std::invoke_result_t<const F&, Args&&...>>) {
    std::invoke(fn, std::forward<Args>(args)...);
    return unit{};
  } else {
    return std::invoke(fn, std::forward<Args>(args)...);
  }
}

/// Invokes a plain callable immediately, returning anything it throws as
/// `Exception` when `Catch` is set.
template <class Out, class Err, bool Catch, class F, class... Args>
Result<Out, Err> call_sync(const F& fn, Args&&... args) {
#if BEET_EXCEPTIONS
  if constexpr (Catch) {
    try {
      return to_result<Out, Err>(invoke_value(fn, std::forward<Args>(args)...));
    } catch (...) {
      return make_unexpected(coerce<Err>(Exception{std::current_exception()}));
    }
  }
#endif
  return to_result<Out, Err>(invoke_value(fn, std::forward<Args>(args)...));
}

template <class Out, class Err, bool Catch, class F, class... Args>
Task<Result<Out, Err>> lazy_call(const F& fn, Args... args) {
  co_return call_sync<Out, Err, Catch>(fn, std::move(args)...);
}

/// Runs a coroutine callable, returning anything it throws as `Exception`.
template <class Out, class Err, class F, class... Args>
Task<Result<Out, Err>> catching_task(const F& fn, Args... args) {
#if BEET_EXCEPTIONS
  std::exception_ptr caught;
  try {
    co_return to_result<Out, Err>(
        co_await settle(std::invoke(std::as_const(fn), std::move(args)...)));
  } catch (...) {
    caught = std::current_exception();
  }
  co_return make_unexpected(coerce<Err>(Exception{caught}));
#else
  co_return to_result<Out, Err>(
      co_await settle(std::invoke(std::as_const(fn), std::move(args)...)));
#endif
}

/// Invokes `fn` as a node body. Plain callables are deferred into a task so
/// they run when first ticked.
template <class F, class... Args>
auto call(const F& fn, Args&&... args) {
  using C = call_traits<F, Args&&...>;
  constexpr bool catches = may_throw_v<F, Args&&...>;
  if constexpr (C::lifted::is_task && !catches) {
    return std::invoke(fn, std::forward<Args>(args)...);
  } else if constexpr (C::lifted::is_task) {
    return catching_task<typename C::out, typename C::err>(
        fn, std::decay_t<Args>(std::forward<Args>(args))...);
  } else {
    return lazy_call<typename C::out, typename C::err, catches>(
        fn, std::decay_t<Args>(std::forward<Args>(args))...);
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
std::function<Task<Result<Out, Err>>(In)> erase(N child) {
  static_assert(
      std::is_same_v<input_t<N>, unit> ||
          std::constructible_from<input_t<N>, In>,
      "beet: AnyNode input is not convertible to the wrapped node's input");
  static_assert(
      error_subset_v<error_t<N>, Err>,
      "beet: AnyNode error set does not cover the wrapped node's errors");
  static_assert(N::rate == 0, "beet: AnyNode cannot hide a rate<K>() region");
  if constexpr (std::is_same_v<input_t<N>, In> &&
                std::is_same_v<result_t<N>, Result<Out, Err>>) {
    return [child = std::move(child)](In in) { return child(std::move(in)); };
  } else {
    return [child = std::move(child)](In in) {
      return adapt<Out, Err>(child(feed<N>(std::move(in))));
    };
  }
}

/// `call_traits` of a leaf callable, which is called with its input only if it
/// takes one.
template <class F, class In,
          bool TakesInput = std::is_invocable_v<const F&, In&&>>
struct leaf_traits : call_traits<F, In&&> {
  static constexpr bool catches = may_throw_v<F, In&&>;
};
template <class F, class In>
struct leaf_traits<F, In, false> : call_traits<F> {
  static constexpr bool catches = may_throw_v<F>;
};

template <class F, class In>
struct lift_impl {
  static constexpr std::string_view kind = "leaf";
  using traits = leaf_traits<F, In>;
  static constexpr bool sync = !traits::lifted::is_task;

  F f;

  template <class Trace>
  [[nodiscard]] auto run(In in, Trace /*unused*/) const {
    if constexpr (std::is_invocable_v<const F&, In>) {
      return call(f, std::move(in));
    } else {
      return call(f);
    }
  }

  template <class Trace>
  [[nodiscard]] Result<typename traits::out, typename traits::err> run_sync(
      In in, Trace /*unused*/) const {
    using Out = typename traits::out;
    using Err = typename traits::err;
    if constexpr (std::is_invocable_v<const F&, In>) {
      return call_sync<Out, Err, traits::catches>(f, std::move(in));
    } else {
      return call_sync<Out, Err, traits::catches>(f);
    }
  }
};

/// Trace used when no observer is attached. Every tracing hook on this path
/// compiles to nothing.
struct untraced {
  template <std::size_t Offset>
  [[nodiscard]] constexpr untraced child() const noexcept {
    return {};
  }
};

/// Trace inside a region ticked `Rate` times per period, wrapping the
/// observer's trace (or `untraced`).
template <std::size_t Rate, class Inner>
struct rated {
  static constexpr std::size_t rate = Rate;

  Inner inner;

  template <std::size_t Offset>
  [[nodiscard]] constexpr auto child() const noexcept {
    auto inner_child = inner.template child<Offset>();
    return rated<Rate, decltype(inner_child)>{inner_child};
  }

  template <class R>
  Task<R> wrap(Task<R> task) const {
    return inner.wrap(std::move(task));
  }

  template <class F>
  auto wrap_sync(F&& run) const {
    return inner.wrap_sync(std::forward<F>(run));
  }
};

template <class Trace>
struct unrated {
  using type = Trace;
  static constexpr std::size_t rate = 0;
};
template <std::size_t Rate, class Inner>
struct unrated<rated<Rate, Inner>> {
  using type = Inner;
  static constexpr std::size_t rate = Rate;
};

/// Ticks per period of the region `Trace` belongs to, or 0 outside any region.
template <class Trace>
inline constexpr std::size_t trace_rate_v = unrated<Trace>::rate;

template <class Trace>
inline constexpr bool is_untraced_v =
    std::is_same_v<typename unrated<Trace>::type, untraced>;

/// `trace`, moved into a region ticked `Rate` times per period.
template <std::size_t Rate, class Trace>
constexpr rated<Rate, Trace> with_rate(const Trace& trace) {
  return {trace};
}
template <std::size_t Rate, std::size_t Outer, class Inner>
constexpr rated<Rate, Inner> with_rate(const rated<Outer, Inner>& trace) {
  return {trace.inner};
}

template <class Impl>
inline constexpr bool is_transparent_v = requires { Impl::transparent; };

/// Whether `Impl` always finishes on the tick it starts, so it can run as a
/// plain call through `run_sync` instead of a coroutine.
template <class Impl>
inline constexpr bool impl_sync_v = [] {
  if constexpr (requires { Impl::sync; }) {
    return Impl::sync;
  } else {
    return false;
  }
}();

template <class Impl>
struct impl_children {
  using type = type_list<>;
};
template <class Impl>
  requires requires { typename Impl::children; }
struct impl_children<Impl> {
  using type = typename Impl::children;
};

/// Least common multiple, where 0 means "no rate" and is the identity.
constexpr std::size_t combine_rates(std::size_t lhs, std::size_t rhs) {
  if (lhs == 0 || rhs == 0) {
    return lhs + rhs;
  }
  return std::lcm(lhs, rhs);
}

/// Ticks per period `Impl` needs: its own rate if it sets one, otherwise the
/// combined rates of its children.
template <class Impl, class Children = typename impl_children<Impl>::type>
struct impl_rate;
template <class Impl, class... Cs>
struct impl_rate<Impl, type_list<Cs...>> {
  static constexpr std::size_t value = [] {
    if constexpr (requires { Impl::rate; }) {
      return Impl::rate;
    } else if constexpr (is_transparent_v<Impl>) {
      return Impl::inner_type::rate;
    } else {
      std::size_t rate = 0;
      ((rate = combine_rates(rate, Cs::rate)), ...);
      return rate;
    }
  }();
};

template <class N>
struct subtree_size;

/// Number of nodes in `Impl`'s subtree, counting itself. Transparent impls
/// (labels) add none.
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
struct subtree_size<Node<Impl, In, Out, Err>>
    : std::integral_constant<std::size_t, impl_subtree_size<Impl>::value> {};

template <class N>
inline constexpr std::size_t subtree_size_v =
    subtree_size<std::remove_cvref_t<N>>::value;

/// Depth-first ID of child `I` relative to its parent.
template <class Impl, std::size_t I,
          class Children = typename impl_children<Impl>::type>
struct child_offset;
template <class Impl, std::size_t I, class... Cs>
struct child_offset<Impl, I, type_list<Cs...>> {
  static constexpr std::size_t value = [] {
    constexpr std::array<std::size_t, sizeof...(Cs)> sizes{
        subtree_size<Cs>::value...};
    std::size_t offset = 1;
    for (std::size_t j = 0; j < I; ++j) {
      offset += sizes[j];
    }
    return offset;
  }();
};

template <class Impl, std::size_t I, class Trace>
constexpr auto child_trace(const Trace& trace) {
  return trace.template child<child_offset<Impl, I>::value>();
}

template <class N, class In, class Trace>
Task<result_t<N>> lazy_run(const N& node, In in, Trace trace);

}  // namespace detail

/// A typed behavior tree node: `In -> Task<Result<Out, Err>>`.
///
/// A node and its `Runner` must outlive any task created from it, since
/// combinator tasks refer back to their children.
template <class Impl, class In, class Out, class Err>
class Node {
 public:
  using input_type = In;
  using output_type = Out;
  using error_type = Err;
  using result_type = Result<Out, Err>;
  using impl_type = Impl;

  /// Whether the whole subtree finishes on the tick it starts. Such nodes run
  /// through `run_sync` without allocating coroutine frames.
  static constexpr bool is_sync = detail::impl_sync_v<Impl>;

  /// Ticks per period the subtree needs: the least common multiple of every
  /// `rate<K>()` region in it, or 0 if there are none.
  static constexpr std::size_t rate = detail::impl_rate<Impl>::value;

  explicit Node(Impl impl) : impl_(std::move(impl)) {}

  /// Type-erases another node into an `AnyNode`, widening its error set if
  /// needed.
  template <node_type Other>
    requires(detail::specialization_of<Impl, std::function> &&
             !std::is_same_v<std::remove_cvref_t<Other>, Node>)
  Node(Other other) : impl_(detail::erase<In, Out, Err>(std::move(other))) {}

  Task<result_type> operator()(In in) const {
    static_assert(rate == 0,
                  "beet: a node containing rate<K>() cannot be called "
                  "directly; compose it with beet combinators instead");
    return run(std::move(in), detail::untraced{});
  }

  /// Runs the node under `trace`. Only traces other than `untraced` report
  /// events to an observer.
  template <class Trace>
  Task<result_type> run(In in, Trace trace) const {
    if constexpr (is_sync) {
      return detail::lazy_run(*this, std::move(in), trace);
    } else if constexpr (detail::is_untraced_v<Trace> ||
                         detail::is_transparent_v<Impl>) {
      return run_impl(std::move(in), trace);
    } else {
      return trace.wrap(run_impl(std::move(in), trace));
    }
  }

  /// Runs a sync node to completion in the caller's frame.
  template <class Trace>
    requires is_sync
  [[nodiscard]] result_type run_sync(In in, Trace trace) const {
    if constexpr (detail::is_untraced_v<Trace> ||
                  detail::is_transparent_v<Impl>) {
      return impl_.run_sync(std::move(in), trace);
    } else {
      return trace.wrap_sync(
          [&] { return impl_.run_sync(std::move(in), trace); });
    }
  }

 private:
  template <class Trace>
  Task<result_type> run_impl(In in, Trace trace) const {
    if constexpr (detail::specialization_of<Impl, std::function>) {
      return impl_(std::move(in));
    } else {
      return impl_.run(std::move(in), trace);
    }
  }

  Impl impl_;
};

template <class In, class Out, class Err>
using AnyNode = Node<std::function<Task<Result<Out, Err>>(In)>, In, Out, Err>;

namespace detail {

/// Defers a sync node into a task, for callers that need one.
template <class N, class In, class Trace>
Task<result_t<N>> lazy_run(const N& node, In in, Trace trace) {
  co_return node.run_sync(std::move(in), trace);
}

/// Runs child `node` for `co_await` in a combinator body. A sync child runs
/// right away in the parent's frame; a failure propagates either way.
template <class N, class In, class Trace>
auto await_child(const N& node, In&& in, Trace trace) {
  if constexpr (N::is_sync) {
    return ready_t<result_t<N>>{node.run_sync(std::forward<In>(in), trace)};
  } else {
    return node.run(std::forward<In>(in), trace);
  }
}

/// Like `await_child`, but `co_await` yields the child's whole `Result`.
template <class N, class In, class Trace>
auto settle_child(const N& node, In&& in, Trace trace) {
  return settle(await_child(node, std::forward<In>(in), trace));
}

}  // namespace detail

/// Lifts a callable into a node with an explicit input type. Use this for
/// generic lambdas.
template <class In, class F>
auto node(F fn) {
  if constexpr (node_type<F>) {
    static_assert(std::is_same_v<input_t<F>, In>,
                  "beet: node<In>() given a node with a different input type");
    return fn;
  } else {
    using Impl = detail::lift_impl<F, In>;
    return Node<Impl, In, typename Impl::traits::out,
                typename Impl::traits::err>(Impl{std::move(fn)});
  }
}

/// Lifts a callable `Out(In)`, `Result<Out, E>(In)`, or `Task<Result<Out,
/// E>>(In)` into a node. Callables taking no argument get `unit` input. Nodes
/// are returned unchanged.
template <class F>
auto node(F fn) {
  if constexpr (node_type<F>) {
    return fn;
  } else {
    return node<detail::callable_input_t<F>>(std::move(fn));
  }
}

}  // namespace beet

#include "beet/combinators.hpp"  // IWYU pragma: export
