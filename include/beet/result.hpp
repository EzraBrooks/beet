#pragma once

#include <tl/expected.hpp>

namespace beet {

template <class T, class E>
using Result = tl::expected<T, E>;

using tl::make_unexpected;
using tl::unexpect;
using tl::unexpected;

/// Value type for nodes that take no input or produce no output.
struct unit {
  friend constexpr bool operator==(unit, unit) noexcept = default;
};

/// The empty error set. It cannot be constructed, so a `Result<T, never>`
/// always holds a value.
struct never {
  never() = delete;
};

}  // namespace beet
