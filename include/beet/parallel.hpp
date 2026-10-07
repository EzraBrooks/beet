#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "beet/executor.hpp"
#include "beet/meta.hpp"
#include "beet/node.hpp"
#include "beet/result.hpp"
#include "beet/task.hpp"

namespace beet {
namespace detail {

struct all_policy {};
struct any_policy {};
template <std::size_t K>
struct at_least_policy {};

template <class Policy, class... Cs>
struct parallel_types;
template <class... Cs>
struct parallel_types<all_policy, Cs...> {
  using out = std::tuple<output_t<Cs>...>;
};
template <class... Cs>
struct parallel_types<any_policy, Cs...> {
  using out = std::variant<output_t<Cs>...>;
};
template <std::size_t K, class... Cs>
struct parallel_types<at_least_policy<K>, Cs...> {
  static_assert(K >= 1 && K <= sizeof...(Cs),
                "beet: parallel_n<K> needs 1 <= K <= number of children");
  using out = std::tuple<std::optional<output_t<Cs>>...>;
};

/// The first child input that is not `unit`; children taking `unit` ignore the
/// parallel input.
template <class... Cs>
struct common_input {
  using type = unit;
};
template <class C, class... Cs>
struct common_input<C, Cs...> {
  using type =
      std::conditional_t<std::is_same_v<input_t<C>, unit>,
                         typename common_input<Cs...>::type, input_t<C>>;
};

template <class Policy>
inline constexpr std::string_view parallel_kind = "parallel_n";
template <>
inline constexpr std::string_view parallel_kind<all_policy> = "parallel_all";
template <>
inline constexpr std::string_view parallel_kind<any_policy> = "parallel_any";

template <class Impl, class... Cs>
struct runner_set {
  std::tuple<TaskRunner<result_t<Cs>>...> runners;
  std::array<runner_base*, sizeof...(Cs)> ptrs;

  template <class In, class Trace>
  runner_set(const std::tuple<Cs...>& children, const In& in, Trace trace)
      : runner_set(children, in, trace, std::index_sequence_for<Cs...>{}) {}

  template <class In, class Trace, std::size_t... I>
  runner_set(const std::tuple<Cs...>& children, const In& in, Trace trace,
             std::index_sequence<I...>)
      : runners(std::get<I>(children).run(feed<Cs>(in),
                                          child_trace<Impl, I>(trace))...),
        ptrs{&std::get<I>(runners)...} {}
};

template <class Policy, class... Cs>
struct parallel_impl {
  static constexpr std::string_view kind = parallel_kind<Policy>;
  using children = type_list<Cs...>;
  using In = typename common_input<Cs...>::type;
  using Out = typename parallel_types<Policy, Cs...>::out;
  using Err = error_union_t<error_t<Cs>...>;
  using R = Result<Out, Err>;
  static constexpr std::size_t count = sizeof...(Cs);

  std::tuple<Cs...> nodes;
  Executor* executor;

  template <class Trace>
  Task<R> run(In in, Trace trace) const {
    runner_set<parallel_impl, Cs...> set(nodes, in, trace);
    for (;;) {
      executor->bulk(count, [&set](std::size_t i) { set.ptrs[i]->tick(); });
      if (auto outcome =
              decide(set.runners, Policy{}, std::index_sequence_for<Cs...>{})) {
        co_return std::move(*outcome);
      }
      co_await running;
    }
  }

  template <class Runners, std::size_t... I>
  static std::optional<R> decide(Runners& rs, all_policy,
                                 std::index_sequence<I...>) {
    std::optional<R> out;
    ((out || std::get<I>(rs).status() != Status::Failure
          ? void()
          : void(out.emplace(
                unexpect,
                coerce<Err>(std::move(std::get<I>(rs).result().error()))))),
     ...);
    if (!out && (... && (std::get<I>(rs).status() == Status::Success))) {
      out.emplace(tl::in_place, std::move(*std::get<I>(rs).result())...);
    }
    return out;
  }

  template <class Runners, std::size_t... I>
  static std::optional<R> decide(Runners& rs, any_policy,
                                 std::index_sequence<I...>) {
    std::optional<R> out;
    ((out || std::get<I>(rs).status() != Status::Success
          ? void()
          : void(out.emplace(tl::in_place, std::in_place_index<I>,
                             std::move(*std::get<I>(rs).result())))),
     ...);
    if (!out && (... && (std::get<I>(rs).status() == Status::Failure))) {
      out.emplace(
          unexpect,
          coerce<Err>(std::move(std::get<count - 1>(rs).result().error())));
    }
    return out;
  }

  template <class Runners, std::size_t K, std::size_t... I>
  static std::optional<R> decide(Runners& rs, at_least_policy<K>,
                                 std::index_sequence<I...>) {
    std::optional<R> out;
    const std::size_t succeeded =
        (std::size_t{0} + ... + (std::get<I>(rs).status() == Status::Success));
    const std::size_t failed =
        (std::size_t{0} + ... + (std::get<I>(rs).status() == Status::Failure));
    if (succeeded >= K) {
      out.emplace(tl::in_place, take_value(std::get<I>(rs))...);
    } else if (failed > count - K) {
      ((std::get<I>(rs).status() == Status::Failure
            ? void(out.emplace(unexpect,
                               coerce<Err>(std::get<I>(rs).result().error())))
            : void()),
       ...);
    }
    return out;
  }

  template <class Runner>
  static auto take_value(Runner& r) {
    using V = typename std::remove_cvref_t<decltype(r.result())>::value_type;
    if (r.status() != Status::Success) return std::optional<V>{};
    return std::optional<V>{std::move(*r.result())};
  }
};

template <class Policy, class... Ns>
auto make_parallel(Executor& executor, Ns... children) {
  static_assert(sizeof...(Ns) >= 1,
                "beet: parallel nodes need at least one child");
  using I = parallel_impl<Policy, Ns...>;
  static_assert(
      ((accepts_any_input_v<Ns> ||
        std::constructible_from<input_t<Ns>, const typename I::In&>) &&
       ...),
      "beet: parallel children must share one input type (or take no input)");
  return Node<I, typename I::In, typename I::Out, typename I::Err>(
      I{{std::move(children)...}, &executor});
}

template <class T>
concept not_executor = !std::derived_from<std::remove_cvref_t<T>, Executor>;

}  // namespace detail

/// Ticks all children every tick. Succeeds with a tuple of their outputs once
/// all succeed; fails with the first failure (lowest index) and halts the rest.
template <class... Cs>
auto parallel_all(Executor& executor, Cs... children) {
  return detail::make_parallel<detail::all_policy>(
      executor, node(std::move(children))...);
}
template <detail::not_executor C, class... Cs>
auto parallel_all(C first, Cs... rest) {
  return parallel_all(inline_executor(), std::move(first), std::move(rest)...);
}

/// Succeeds with the first child to succeed (lowest index on ties), as a
/// variant indexed by child, and halts the rest. Fails with the last child's
/// error once every child has failed.
template <class... Cs>
auto parallel_any(Executor& executor, Cs... children) {
  return detail::make_parallel<detail::any_policy>(
      executor, node(std::move(children))...);
}
template <detail::not_executor C, class... Cs>
auto parallel_any(C first, Cs... rest) {
  return parallel_any(inline_executor(), std::move(first), std::move(rest)...);
}

/// Succeeds once at least `K` children succeed, with each child's output if it
/// succeeded. Fails once success becomes impossible, with the highest-index
/// failure, and halts the rest.
template <std::size_t K, class... Cs>
auto parallel_n(Executor& executor, Cs... children) {
  return detail::make_parallel<detail::at_least_policy<K>>(
      executor, node(std::move(children))...);
}
template <std::size_t K, detail::not_executor C, class... Cs>
auto parallel_n(C first, Cs... rest) {
  return parallel_n<K>(inline_executor(), std::move(first), std::move(rest)...);
}

}  // namespace beet
