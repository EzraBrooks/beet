#pragma once

#include <exception>
#include <string>

#include <tl/expected.hpp>

/// Nonzero when beet catches exceptions thrown by user callables. Compiling
/// without exception support, or defining `BEET_NO_EXCEPTIONS`, turns it off.
#if defined(__cpp_exceptions) && !defined(BEET_NO_EXCEPTIONS)
#define BEET_EXCEPTIONS 1
#else
#define BEET_EXCEPTIONS 0
#endif

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

/// An exception thrown by a callable that is not `noexcept`, caught and
/// returned as an error.
struct Exception {
  std::exception_ptr ptr;

  /// The exception's `what()`, if it derives from `std::exception`.
  std::string what() const {
#if BEET_EXCEPTIONS
    try {
      std::rethrow_exception(ptr);
    } catch (const std::exception& e) {
      return e.what();
    } catch (...) {
      return "unknown exception";
    }
#else
    return "unknown exception";
#endif
  }
};

}  // namespace beet
