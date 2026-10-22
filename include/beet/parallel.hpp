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
struct at_least_policy {
  static constexpr std::size_t required = K;
};

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

  template <class In, class Trace>
  runner_set(const std::tuple<Cs...>& children, const In& in, Trace trace)
      : runner_set(children, in, trace, std::index_sequence_for<Cs...>{}) {}

  template <class In, class Trace, std::size_t... I>
  runner_set(const std::tuple<Cs...>& children, const In& in, Trace trace,
             std::index_sequence<I...> /*unused*/)
      : runners(std::get<I>(children).run(feed<Cs>(in),
                                          child_trace<Impl, I>(trace))...) {}
};

template <class Exec>
struct executor_handle {
  explicit executor_handle(Exec& exec) : ptr(&exec) {}
  [[nodiscard]] Exec& get() const { return *ptr; }

  Exec* ptr;
};
template <class Exec>
  requires std::is_empty_v<Exec>
struct executor_handle<Exec> {
  explicit executor_handle(const Exec& exec) : exec(exec) {}
  [[nodiscard]] Exec& get() const { return exec; }

  [[no_unique_address]] mutable Exec exec;
};

/// `Exec` is the executor's concrete type, so its `bulk` call is direct and
/// `InlineExecutor` needs no indirection at all.
template <class Policy, class Exec, class... Cs>
struct parallel_impl {
  static constexpr std::string_view kind = parallel_kind<Policy>;
  using children = type_list<Cs...>;
  using In = typename common_input<Cs...>::type;
  using Out = typename parallel_types<Policy, Cs...>::out;
  using Err = error_union_t<error_t<Cs>...>;
  using R = Result<Out, Err>;
  static constexpr std::size_t count = sizeof...(Cs);

  std::tuple<Cs...> nodes;
  [[no_unique_address]] executor_handle<Exec> executor;

  template <class Trace>
  Task<R> run(In in, Trace trace) const {
    runner_set<parallel_impl, Cs...> set(nodes, in, trace);
    for (;;) {
      tick_all(set.runners);
      if (auto outcome = decide(set.runners)) {
        co_return std::move(*outcome);
      }
      co_await running;
    }
  }

  template <class Runners>
  void tick_all(Runners& runners) const {
    if constexpr (std::is_same_v<Exec, InlineExecutor>) {
      std::apply([](auto&... each) { ((void)each.tick(), ...); }, runners);
    } else {
      executor.get().bulk(
          count, [&runners](std::size_t index) { tick_one(runners, index); });
    }
  }

  template <class Runners>
  static void tick_one(Runners& runners, std::size_t index) {
    static constexpr auto table =
        []<std::size_t... I>(std::index_sequence<I...> /*unused*/) {
          return std::array{
              +[](Runners& each) { (void)std::get<I>(each).tick(); }...};
        }(std::index_sequence_for<Cs...>{});
    table[index](runners);
  }

  template <class Runners>
  static std::optional<R> decide(Runners& runners) {
    return [&]<std::size_t... I>(std::index_sequence<I...> /*unused*/) {
      if constexpr (std::is_same_v<Policy, all_policy>) {
        return decide_all<I...>(runners);
      } else if constexpr (std::is_same_v<Policy, any_policy>) {
        return decide_any<I...>(runners);
      } else {
        return decide_at_least<I...>(runners);
      }
    }(std::index_sequence_for<Cs...>{});
  }

  template <std::size_t... I, class Runners>
  static std::optional<R> decide_all(Runners& runners) {
    std::optional<R> out;
    (
        [&] {
          if (!out && std::get<I>(runners).status() == Status::Failure) {
            out.emplace(
                unexpect,
                coerce<Err>(std::move(std::get<I>(runners).result().error())));
          }
        }(),
        ...);
    if (!out && (... && (std::get<I>(runners).status() == Status::Success))) {
      out.emplace(tl::in_place, std::move(*std::get<I>(runners).result())...);
    }
    return out;
  }

  template <std::size_t... I, class Runners>
  static std::optional<R> decide_any(Runners& runners) {
    std::optional<R> out;
    (
        [&] {
          if (!out && std::get<I>(runners).status() == Status::Success) {
            out.emplace(tl::in_place, std::in_place_index<I>,
                        std::move(*std::get<I>(runners).result()));
          }
        }(),
        ...);
    if (!out && (... && (std::get<I>(runners).status() == Status::Failure))) {
      out.emplace(unexpect,
                  coerce<Err>(std::move(
                      std::get<count - 1>(runners).result().error())));
    }
    return out;
  }

  template <std::size_t... I, class Runners>
  static std::optional<R> decide_at_least(Runners& runners) {
    constexpr std::size_t required = Policy::required;
    std::optional<R> out;
    const std::size_t succeeded =
        (std::size_t{0} + ... +
         (std::get<I>(runners).status() == Status::Success));
    const std::size_t failed =
        (std::size_t{0} + ... +
         (std::get<I>(runners).status() == Status::Failure));
    if (succeeded >= required) {
      out.emplace(tl::in_place, take_value(std::get<I>(runners))...);
    } else if (failed > count - required) {
      (
          [&] {
            if (std::get<I>(runners).status() == Status::Failure) {
              out.emplace(unexpect,
                          coerce<Err>(std::get<I>(runners).result().error()));
            }
          }(),
          ...);
    }
    return out;
  }

  template <class Runner>
  static auto take_value(Runner& runner) {
    using V =
        typename std::remove_cvref_t<decltype(runner.result())>::value_type;
    if (runner.status() != Status::Success) {
      return std::optional<V>{};
    }
    return std::optional<V>{std::move(*runner.result())};
  }
};

template <class Policy, class Exec, class... Ns>
auto make_parallel(Exec& executor, Ns... children) {
  static_assert(sizeof...(Ns) >= 1,
                "beet: parallel nodes need at least one child");
  using I = parallel_impl<Policy, Exec, Ns...>;
  static_assert(
      ((accepts_any_input_v<Ns> ||
        std::constructible_from<input_t<Ns>, const typename I::In&>) &&
       ...),
      "beet: parallel children must share one input type (or take no input)");
  return Node<I, typename I::In, typename I::Out, typename I::Err>(
      I{{std::move(children)...}, executor_handle<Exec>(executor)});
}

template <class T>
concept not_executor = !executor<std::remove_cvref_t<T>>;

}  // namespace detail

/// Ticks all children every tick. Succeeds with a tuple of their outputs once
/// all succeed; fails with the first failure (lowest index) and halts the rest.
template <executor E, class... Cs>
auto parallel_all(E& executor, Cs... children) {
  return detail::make_parallel<detail::all_policy>(
      executor, node(std::move(children))...);
}
template <detail::not_executor C, class... Cs>
auto parallel_all(C first, Cs... rest) {
  InlineExecutor inline_exec;
  return parallel_all(inline_exec, std::move(first), std::move(rest)...);
}

/// Succeeds with the first child to succeed (lowest index on ties), as a
/// variant indexed by child, and halts the rest. Fails with the last child's
/// error once every child has failed.
template <executor E, class... Cs>
auto parallel_any(E& executor, Cs... children) {
  return detail::make_parallel<detail::any_policy>(
      executor, node(std::move(children))...);
}
template <detail::not_executor C, class... Cs>
auto parallel_any(C first, Cs... rest) {
  InlineExecutor inline_exec;
  return parallel_any(inline_exec, std::move(first), std::move(rest)...);
}

/// Succeeds once at least `K` children succeed, with each child's output if it
/// succeeded. Fails once success becomes impossible, with the highest-index
/// failure, and halts the rest.
template <std::size_t K, executor E, class... Cs>
auto parallel_n(E& executor, Cs... children) {
  return detail::make_parallel<detail::at_least_policy<K>>(
      executor, node(std::move(children))...);
}
template <std::size_t K, detail::not_executor C, class... Cs>
auto parallel_n(C first, Cs... rest) {
  InlineExecutor inline_exec;
  return parallel_n<K>(inline_exec, std::move(first), std::move(rest)...);
}

}  // namespace beet
