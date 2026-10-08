#include <doctest/doctest.h>

#include <stdexcept>
#include <variant>

#include "beet/beet.hpp"

namespace {

using beet::Exception;
using beet::never;
using beet::Result;
using beet::Status;
using beet::Task;

struct Bad {};

int may_throw(int x) {
  if (x < 0) throw std::runtime_error("negative");
  return x;
}
int cannot_throw(int x) noexcept { return x; }

Task<Result<int, Bad>> throws_later(int x) {
  co_await beet::running;
  if (x < 0) throw std::runtime_error("late");
  co_return x;
}

template <class N>
auto run(const N& n, beet::input_t<N> in) {
  beet::Runner r{n, std::move(in)};
  while (r.tick() == Status::Running) {
  }
  return r.result();
}

}  // namespace

TEST_CASE("a callable that is not noexcept adds Exception to its error set") {
  auto tree = beet::node(may_throw);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, Exception>);
  auto safe = beet::node(cannot_throw);
  static_assert(std::is_same_v<beet::error_t<decltype(safe)>, never>);

  CHECK(run(safe, 3).value() == 3);
  CHECK(run(tree, 3).value() == 3);
  CHECK(run(tree, -3).error().what() == "negative");
}

TEST_CASE("exceptions thrown by a coroutine leaf become errors") {
  auto tree = beet::node(throws_later);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>,
                               std::variant<Bad, Exception>>);
  CHECK(run(tree, 1).value() == 1);
  CHECK(std::get<Exception>(run(tree, -1).error()).what() == "late");
}

TEST_CASE("recover<Exception> handles thrown exceptions like any error") {
  auto tree = beet::recover<Exception>(
      may_throw, [](const Exception&) noexcept { return 0; });
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, never>);
  CHECK(run(tree, -3).value() == 0);
}

TEST_CASE("a recover handler that may throw adds Exception back") {
  auto tree = beet::recover(may_throw, [](const Exception& e) -> int {
    throw std::logic_error("handler saw " + e.what());
  });
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, Exception>);
  CHECK(run(tree, -3).error().what() == "handler saw negative");
}

TEST_CASE("a thrown value that is not a std::exception is still caught") {
  auto tree = beet::node([](int) -> int { throw 42; });
  CHECK(run(tree, 0).error().what() == "unknown exception");
}
