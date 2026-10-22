#pragma once

// Opt-in tracing. Nothing in this header is reachable from an untraced
// `Runner`, so trees run without listeners contain no tracing code at all.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include "beet/node.hpp"
#include "beet/runner.hpp"
#include "beet/scope_exit.hpp"
#include "beet/task.hpp"

namespace beet {

/// The hook a `Listeners` leaves unset.
struct ignore_event {
  constexpr void operator()(const auto&... /*unused*/) const noexcept {}
};

/// Callbacks for a `Runner`'s node events, by depth-first node ID (see
/// `describe`). Set only the hooks you need; the rest compile to nothing:
///
///   beet::Listeners listeners{.on_halt = [](std::uint32_t id) { ... }};
///
/// Hooks must be thread-safe when the tree uses a `ThreadPoolExecutor`.
template <class TickBegin = ignore_event, class TickEnd = ignore_event,
          class Start = ignore_event, class Finish = ignore_event,
          class Halt = ignore_event>
struct Listeners {
  TickBegin on_tick_begin{};  // (std::uint64_t tick)
  TickEnd on_tick_end{};      // (std::uint64_t tick, Status outcome)
  Start on_start{};           // (std::uint32_t id)
  Finish on_finish{};         // (std::uint32_t id, Status outcome)
  Halt on_halt{};             // (std::uint32_t id)
};

inline constexpr std::uint32_t no_parent =
    std::numeric_limits<std::uint32_t>::max();

struct node_info {
  std::string_view kind;
  std::string_view label;
  std::string_view input;
  std::string_view output;
  std::string_view error;
  std::uint32_t parent = no_parent;
};

namespace detail {

template <class T>
consteval std::string_view raw_type_name() {
#if defined(__clang__) || defined(__GNUC__)
  constexpr std::string_view fn = __PRETTY_FUNCTION__;
  constexpr std::size_t start = fn.find("T = ") + 4;
  constexpr std::size_t semi = fn.find(';', start);
  constexpr std::size_t end =
      semi == std::string_view::npos ? fn.rfind(']') : semi;
#elif defined(_MSC_VER)
  constexpr std::string_view fn = __FUNCSIG__;
  constexpr std::size_t start = fn.find("raw_type_name<") + 14;
  constexpr std::size_t end = fn.rfind(">(void)");
#else
#error "beet: unsupported compiler for type names"
#endif
  return fn.substr(start, end - start);
}

template <class T>
struct type_name_holder {
  static constexpr std::string_view raw = raw_type_name<T>();
  static constexpr auto chars = [] {
    std::array<char, raw.size()> out{};
    std::ranges::copy(raw, out.begin());
    return out;
  }();
};

template <class Impl>
constexpr std::string_view impl_kind() {
  if constexpr (requires { Impl::kind; }) {
    return Impl::kind;
  } else if constexpr (specialization_of<Impl, std::function>) {
    return "any";
  } else {
    return "node";
  }
}

}  // namespace detail

/// Compile-time spelling of `T`, e.g. `std::tuple<Odom, Level>`.
/// Compiler-specific formatting.
template <class T>
consteval std::string_view type_name() {
  return {detail::type_name_holder<T>::chars.data(),
          detail::type_name_holder<T>::chars.size()};
}

namespace detail {

template <class N, std::size_t S>
constexpr void describe_into(std::array<node_info, S>& out, std::uint32_t id,
                             std::uint32_t parent, std::string_view label) {
  using Impl = typename N::impl_type;
  if constexpr (is_transparent_v<Impl>) {
    describe_into<typename Impl::inner_type>(
        out, id, parent, label.empty() ? Impl::label : label);
  } else {
    out[id] = {impl_kind<Impl>(),       label,
               type_name<input_t<N>>(), type_name<output_t<N>>(),
               type_name<error_t<N>>(), parent};
    using Children = typename impl_children<Impl>::type;
    [&]<class... Cs>(type_list<Cs...> /*unused*/) {
      [&]<std::size_t... I>(std::index_sequence<I...> /*unused*/) {
        (describe_into<Cs>(
             out, static_cast<std::uint32_t>(id + child_offset<Impl, I>::value),
             id, {}),
         ...);
      }(std::index_sequence_for<Cs...>{});
    }(Children{});
  }
}

}  // namespace detail

/// The tree's nodes in depth-first order, indexed by the IDs listeners receive.
/// The root is ID 0.
template <node_type Tree>
consteval auto describe() {
  std::array<node_info, detail::subtree_size_v<Tree>> out{};
  detail::describe_into<std::remove_cvref_t<Tree>>(out, 0, no_parent, {});
  return out;
}

template <node_type Tree>
inline constexpr auto tree_info = describe<Tree>();

namespace detail {

template <class R, class Hooks>
Task<R> observe(Task<R> inner, Hooks* hooks, std::uint32_t id) {
  hooks->on_start(id);
  scope_exit on_halt{[hooks, id] { hooks->on_halt(id); }};
  R result = co_await settle(std::move(inner));
  on_halt.release();
  hooks->on_finish(id, result.has_value() ? Status::Success : Status::Failure);
  co_return std::move(result);
}

template <class L>
concept valid_listeners =
    requires(L& hooks, std::uint32_t id, Status outcome, std::uint64_t tick) {
      hooks.on_tick_begin(tick);
      hooks.on_tick_end(tick, outcome);
      hooks.on_start(id);
      hooks.on_finish(id, outcome);
      hooks.on_halt(id);
    };

template <class Hooks>
struct traced {
  static_assert(specialization_of<Hooks, Listeners>,
                "beet: a Runner's third argument must be a beet::Listeners");
  static_assert(valid_listeners<Hooks>,
                "beet: a beet::Listeners hook cannot be called with its "
                "event's arguments");

  Hooks* hooks;
  std::uint32_t id;

  template <std::size_t Offset>
  [[nodiscard]] constexpr traced child() const noexcept {
    return {hooks, static_cast<std::uint32_t>(id + Offset)};
  }

  template <class R>
  Task<R> wrap(Task<R> inner) const {
    return observe(std::move(inner), hooks, id);
  }

  template <class F>
  auto wrap_sync(F&& run) const {
    hooks->on_start(id);
    scope_exit on_halt{[this] { hooks->on_halt(id); }};
    auto result = std::forward<F>(run)();
    on_halt.release();
    hooks->on_finish(id,
                     result.has_value() ? Status::Success : Status::Failure);
    return result;
  }
};

}  // namespace detail

enum class NodeStatus : std::uint8_t {
  Idle,
  Running,
  Success,
  Failure,
  Halted
};

constexpr std::string_view to_string(NodeStatus status) {
  switch (status) {
    using enum NodeStatus;
    case Idle: return "idle";
    case Running: return "running";
    case Success: return "success";
    case Failure: return "failure";
    case Halted: return "halted";
  }
  return "unknown";
}

/// Latest status of every node in `Tree`, and the tick on which it last
/// changed.
template <node_type Tree>
class StatusTable {
 public:
  static constexpr auto nodes = describe<Tree>();
  static constexpr std::size_t size = nodes.size();

  /// Hooks that keep this table up to date, for a `Runner`. The table must
  /// outlive the `Runner`.
  [[nodiscard]] auto listeners() {
    return Listeners{
        .on_tick_begin =
            [this](std::uint64_t tick) {
              tick_.store(tick, std::memory_order_relaxed);
            },
        .on_start = [this](std::uint32_t id) { set(id, NodeStatus::Running); },
        .on_finish =
            [this](std::uint32_t id, Status outcome) {
              set(id, outcome == Status::Success ? NodeStatus::Success
                                                 : NodeStatus::Failure);
            },
        .on_halt = [this](std::uint32_t id) { set(id, NodeStatus::Halted); }};
  }

  [[nodiscard]] NodeStatus status(std::uint32_t id) const {
    return status_[id].load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t changed_at(std::uint32_t id) const {
    return changed_[id].load(std::memory_order_relaxed);
  }

 private:
  void set(std::uint32_t id, NodeStatus value) {
    status_[id].store(value, std::memory_order_relaxed);
    changed_[id].store(tick_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
  }

  std::atomic<std::uint64_t> tick_{0};
  std::array<std::atomic<NodeStatus>, size> status_{};
  std::array<std::atomic<std::uint64_t>, size> changed_{};
};

}  // namespace beet
