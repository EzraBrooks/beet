#pragma once

// Opt-in tracing. Nothing in this header is reachable from an untraced `Runner`, so trees run
// without an observer contain no tracing code at all.

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include "beet/node.hpp"
#include "beet/runner.hpp"
#include "beet/task.hpp"

namespace beet {

/// Receives node events by depth-first node ID (see `describe`). Must be thread-safe when the tree
/// uses a `ThreadPoolExecutor`.
template <class O>
concept observer = requires(O& o, std::uint32_t id, Status s, std::uint64_t tick) {
  o.on_tick_begin(tick);
  o.on_tick_end(tick, s);
  o.on_start(id);
  o.on_finish(id, s);
  o.on_halt(id);
};

inline constexpr std::uint32_t no_parent = std::numeric_limits<std::uint32_t>::max();

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
constexpr std::string_view raw_type_name() {
#if defined(__clang__) || defined(__GNUC__)
  constexpr std::string_view fn = __PRETTY_FUNCTION__;
  constexpr std::size_t start = fn.find("T = ") + 4;
  constexpr std::size_t semi = fn.find(';', start);
  constexpr std::size_t end = semi == std::string_view::npos ? fn.rfind(']') : semi;
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
    for (std::size_t i = 0; i < raw.size(); ++i) out[i] = raw[i];
    return out;
  }();
};

template <class Impl>
constexpr std::string_view impl_kind() {
  if constexpr (requires { Impl::kind; }) {
    return Impl::kind;
  } else if constexpr (is_std_function<Impl>::value) {
    return "any";
  } else {
    return "node";
  }
}

template <class N, std::size_t S>
constexpr void describe_into(std::array<node_info, S>& out, std::uint32_t id, std::uint32_t parent,
                             std::string_view label);

template <class Impl, std::size_t S, class... Cs, std::size_t... I>
constexpr void describe_children(std::array<node_info, S>& out, [[maybe_unused]] std::uint32_t id, type_list<Cs...>,
                                 std::index_sequence<I...>) {
  (describe_into<Cs>(out, static_cast<std::uint32_t>(id + child_offset<Impl, I>::value), id, {}), ...);
}

}  // namespace detail

/// Compile-time spelling of `T`, e.g. `std::tuple<Odom, Level>`. Compiler-specific formatting.
template <class T>
constexpr std::string_view type_name() {
  return {detail::type_name_holder<T>::chars.data(), detail::type_name_holder<T>::chars.size()};
}

namespace detail {

template <class N, std::size_t S>
constexpr void describe_into(std::array<node_info, S>& out, std::uint32_t id, std::uint32_t parent,
                             std::string_view label) {
  using Impl = typename N::impl_type;
  if constexpr (is_transparent_v<Impl>) {
    describe_into<typename Impl::inner_type>(out, id, parent, label.empty() ? Impl::label : label);
  } else {
    out[id] = {impl_kind<Impl>(), label, type_name<input_t<N>>(), type_name<output_t<N>>(), type_name<error_t<N>>(),
               parent};
    using Children = typename impl_children<Impl>::type;
    [&]<class... Cs>(type_list<Cs...> children) {
      describe_children<Impl>(out, id, children, std::index_sequence_for<Cs...>{});
    }(Children{});
  }
}

}  // namespace detail

/// The tree's nodes in depth-first order, indexed by the IDs observers receive. The root is ID 0.
template <node_type Tree>
constexpr auto describe() {
  std::array<node_info, detail::subtree_size_v<Tree>> out{};
  detail::describe_into<std::remove_cvref_t<Tree>>(out, 0, no_parent, {});
  return out;
}

template <node_type Tree>
inline constexpr auto tree_info = describe<Tree>();

namespace detail {

template <class Obs>
struct halt_guard {
  Obs* observer;
  std::uint32_t id;
  bool finished = false;

  ~halt_guard() {
    if (!finished) observer->on_halt(id);
  }
};

template <class R, class Obs>
Task<R> observe(Task<R> inner, Obs* observer, std::uint32_t id) {
  observer->on_start(id);
  halt_guard<Obs> guard{observer, id};
  R result = co_await settle(std::move(inner));
  guard.finished = true;
  observer->on_finish(id, result.has_value() ? Status::Success : Status::Failure);
  co_return std::move(result);
}

template <class Obs>
struct traced {
  static_assert(observer<Obs>, "beet: Runner observer does not satisfy beet::observer");

  Obs* obs;
  std::uint32_t id;

  template <std::size_t Offset>
  constexpr traced child() const noexcept {
    return {obs, static_cast<std::uint32_t>(id + Offset)};
  }

  template <class R>
  Task<R> wrap(Task<R> inner) const {
    return observe(std::move(inner), obs, id);
  }
};

}  // namespace detail

enum class NodeStatus : std::uint8_t { Idle, Running, Success, Failure, Halted };

constexpr std::string_view to_string(NodeStatus s) {
  switch (s) {
    case NodeStatus::Idle: return "idle";
    case NodeStatus::Running: return "running";
    case NodeStatus::Success: return "success";
    case NodeStatus::Failure: return "failure";
    case NodeStatus::Halted: return "halted";
  }
  return "unknown";
}

/// Latest status of every node in `Tree`, and the tick on which it last changed.
template <node_type Tree>
class StatusTable {
 public:
  static constexpr auto nodes = describe<Tree>();
  static constexpr std::size_t size = nodes.size();

  void on_tick_begin(std::uint64_t tick) { tick_.store(tick, std::memory_order_relaxed); }
  void on_tick_end(std::uint64_t, Status) {}
  void on_start(std::uint32_t id) { set(id, NodeStatus::Running); }
  void on_finish(std::uint32_t id, Status s) {
    set(id, s == Status::Success ? NodeStatus::Success : NodeStatus::Failure);
  }
  void on_halt(std::uint32_t id) { set(id, NodeStatus::Halted); }

  NodeStatus status(std::uint32_t id) const { return status_[id].load(std::memory_order_relaxed); }
  std::uint64_t changed_at(std::uint32_t id) const { return changed_[id].load(std::memory_order_relaxed); }

 private:
  void set(std::uint32_t id, NodeStatus s) {
    status_[id].store(s, std::memory_order_relaxed);
    changed_[id].store(tick_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }

  std::atomic<std::uint64_t> tick_{0};
  std::array<std::atomic<NodeStatus>, size> status_{};
  std::array<std::atomic<std::uint64_t>, size> changed_{};
};

}  // namespace beet
