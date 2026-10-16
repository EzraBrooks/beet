#include <doctest/doctest.h>

#include <string>
#include <variant>
#include <vector>

#include "beet/beet.hpp"

namespace {

using beet::input_t;
using beet::never;
using beet::output_t;
using beet::Result;
using beet::Status;
using beet::Task;

struct TooBig {
  int value;
};
struct Negative {};
struct Rewritten {
  std::string why;
};

Result<int, Negative> non_negative(int value) noexcept {
  if (value < 0) {
    return beet::make_unexpected(Negative{});
  }
  return value;
}
Result<int, TooBig> small(int value) noexcept {
  if (value > 100) {
    return beet::make_unexpected(TooBig{value});
  }
  return value;
}
std::string show(int value) noexcept { return std::to_string(value); }

Task<Result<int, never>> slow_add_one(int value) noexcept {
  co_await beet::running;
  co_return value + 1;
}

template <class N>
auto run(const N& node, input_t<N> in) {
  beet::Runner runner{node, std::move(in)};
  while (runner.tick() == Status::Running) {
  }
  return runner.result();
}

}  // namespace

TEST_CASE("sequence pipes outputs into inputs and unions the error sets") {
  auto tree = beet::sequence(non_negative, small, show);
  static_assert(std::is_same_v<output_t<decltype(tree)>, std::string>);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>,
                               std::variant<Negative, TooBig>>);

  CHECK(run(tree, 42).value() == "42");
  CHECK(std::holds_alternative<Negative>(run(tree, -1).error()));
  CHECK(std::get<TooBig>(run(tree, 500).error()).value == 500);
}

TEST_CASE("nested sequences run like a flat one") {
  auto twice = [](int value) { return value * 2; };
  auto nested = beet::sequence(beet::sequence(non_negative, small),
                               beet::sequence(twice, show));
  CHECK(run(nested, 21).value() == "42");
  CHECK(std::holds_alternative<Negative>(run(nested, -1).error()));
}

TEST_CASE(
    "sequence can drop the previous output when the next node takes no input") {
  int calls = 0;
  auto tree = beet::sequence(non_negative, [&calls] { return ++calls; });
  CHECK(run(tree, 3).value() == 1);
  CHECK_FALSE(run(tree, -3).has_value());
  CHECK(calls == 1);
}

TEST_CASE("sequence waits on running children") {
  auto tree = beet::sequence(slow_add_one, slow_add_one);
  beet::Runner runner{tree, 0};
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Success);
  CHECK(runner.result().value() == 2);
}

TEST_CASE("recover<E> removes only the handled error type") {
  auto tree = beet::recover<TooBig>(
      beet::sequence(non_negative, small),
      [](TooBig error) noexcept { return error.value / 10; });
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, Negative>);
  CHECK(run(tree, 500).value() == 50);
  CHECK_FALSE(run(tree, -1).has_value());
}

TEST_CASE(
    "recover with no listed types handles everything and yields an infallible "
    "node") {
  auto tree = beet::recover(beet::sequence(non_negative, small),
                            [](const auto&) noexcept { return 0; });
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, never>);
  CHECK(run(tree, -1).value() == 0);
  CHECK(run(tree, 500).value() == 0);
  CHECK(run(tree, 5).value() == 5);
}

TEST_CASE("a recover handler returning Result translates errors") {
  auto tree = beet::recover(
      non_negative, [](Negative) noexcept -> Result<int, Rewritten> {
        return beet::make_unexpected(Rewritten{"negative input"});
      });
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, Rewritten>);
  CHECK(run(tree, -2).error().why == "negative input");
  CHECK(run(tree, 2).value() == 2);
}

TEST_CASE("a recover handler returning Result can also succeed implicitly") {
  auto tree =
      beet::recover(small, [](TooBig error) noexcept -> Result<int, Rewritten> {
        if (error.value < 1000) {
          return 100;
        }
        return beet::make_unexpected(Rewritten{"way too big"});
      });
  CHECK(run(tree, 500).value() == 100);
  CHECK(run(tree, 5000).error().why == "way too big");
}

TEST_CASE("a recover handler can be a coroutine") {
  auto tree =
      beet::recover(non_negative, [](Negative) -> Task<Result<int, never>> {
        co_await beet::running;
        co_return -1;
      });
  beet::Runner runner{tree, -5};
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Success);
  CHECK(runner.result().value() == -1);
}

TEST_CASE("fallback tries the next alternative on the same input") {
  auto tree =
      beet::fallback(small, [](int value) noexcept -> Result<int, Negative> {
        if (value < 0) {
          return beet::make_unexpected(Negative{});
        }
        return 100;
      });
  static_assert(std::is_same_v<output_t<decltype(tree)>, int>);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, Negative>);
  CHECK(run(tree, 5).value() == 5);
  CHECK(run(tree, 500).value() == 100);
}

TEST_CASE("fallback with differing outputs yields a variant") {
  auto tree = beet::fallback(small, show);
  static_assert(
      std::is_same_v<output_t<decltype(tree)>, std::variant<int, std::string>>);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, never>);
  CHECK(std::get<int>(run(tree, 5).value()) == 5);
  CHECK(std::get<std::string>(run(tree, 500).value()) == "500");
}

TEST_CASE("fallback tries every alternative in order") {
  auto tree = beet::fallback(non_negative, small, [](int) { return 0; });
  CHECK(run(tree, -5).value() == -5);
}

TEST_CASE("finally runs on success, failure, and halt") {
  int calls = 0;
  auto tree = beet::finally(beet::sequence(non_negative, slow_add_one),
                            [&calls] { ++calls; });

  CHECK(run(tree, 1).value() == 2);
  CHECK(calls == 1);
  CHECK_FALSE(run(tree, -1).has_value());
  CHECK(calls == 2);

  beet::Runner runner{tree, 1};
  CHECK(runner.tick() == Status::Running);
  runner.halt();
  CHECK(calls == 3);
}

TEST_CASE("coroutine nodes can await composed subtrees directly") {
  auto validated = beet::sequence(non_negative, small);
  auto tree =
      beet::node([&validated](std::vector<int> values)
                     -> Task<Result<int, std::variant<Negative, TooBig>>> {
        int total = 0;
        for (int value : values) {
          total += co_await validated(value);
        }
        co_return total;
      });
  CHECK(run(tree, {1, 2, 3}).value() == 6);
  CHECK(std::holds_alternative<TooBig>(run(tree, {1, 200, -3}).error()));
}
