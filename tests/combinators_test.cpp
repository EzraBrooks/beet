#include <doctest/doctest.h>

#include <string>
#include <variant>
#include <vector>

#include "beet/beet.hpp"

namespace {

using beet::error_t;
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

Result<int, Negative> non_negative(int x) {
  if (x < 0) return beet::make_unexpected(Negative{});
  return x;
}
Result<int, TooBig> small(int x) {
  if (x > 100) return beet::make_unexpected(TooBig{x});
  return x;
}
std::string show(int x) { return std::to_string(x); }

Task<Result<int, never>> slow_add_one(int x) {
  co_await beet::running;
  co_return x + 1;
}

template <class N>
auto run(const N& n, input_t<N> in) {
  beet::Runner r{n, std::move(in)};
  while (r.tick() == Status::Running) {
  }
  return r.result();
}

}  // namespace

TEST_CASE("then pipes outputs into inputs and unions the error sets") {
  auto tree = beet::node(non_negative).then(small).then(show);
  static_assert(std::is_same_v<output_t<decltype(tree)>, std::string>);
  static_assert(std::is_same_v<error_t<decltype(tree)>, std::variant<Negative, TooBig>>);

  CHECK(run(tree, 42).value() == "42");
  CHECK(std::holds_alternative<Negative>(run(tree, -1).error()));
  CHECK(std::get<TooBig>(run(tree, 500).error()).value == 500);
}

TEST_CASE("pipe operator and sequence are spellings of then") {
  auto piped = beet::node(non_negative) | small | show;
  auto seq = beet::sequence(non_negative, small, show);
  static_assert(std::is_same_v<error_t<decltype(piped)>, error_t<decltype(seq)>>);
  CHECK(run(piped, 7).value() == "7");
  CHECK(run(seq, 7).value() == "7");
}

TEST_CASE("then can drop the previous output when the next node takes no input") {
  int calls = 0;
  auto tree = beet::node(non_negative).then([&calls] { return ++calls; });
  CHECK(run(tree, 3).value() == 1);
  CHECK_FALSE(run(tree, -3).has_value());
  CHECK(calls == 1);
}

TEST_CASE("then waits on running children") {
  auto tree = beet::node(slow_add_one).then(slow_add_one);
  beet::Runner r{tree, 0};
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Success);
  CHECK(r.result().value() == 2);
}

TEST_CASE("recover<E> removes only the handled error type") {
  auto tree = beet::node(non_negative).then(small).recover<TooBig>([](TooBig t) { return t.value / 10; });
  static_assert(std::is_same_v<error_t<decltype(tree)>, Negative>);
  CHECK(run(tree, 500).value() == 50);
  CHECK_FALSE(run(tree, -1).has_value());
}

TEST_CASE("recover with no listed types handles everything and yields an infallible node") {
  auto tree = beet::node(non_negative).then(small).recover([](const auto&) { return 0; });
  static_assert(std::is_same_v<error_t<decltype(tree)>, never>);
  CHECK(run(tree, -1).value() == 0);
  CHECK(run(tree, 500).value() == 0);
  CHECK(run(tree, 5).value() == 5);
}

TEST_CASE("a recover handler returning Result translates errors") {
  auto tree = beet::node(non_negative).recover([](Negative) -> Result<int, Rewritten> {
    return beet::make_unexpected(Rewritten{"negative input"});
  });
  static_assert(std::is_same_v<error_t<decltype(tree)>, Rewritten>);
  CHECK(run(tree, -2).error().why == "negative input");
  CHECK(run(tree, 2).value() == 2);
}

TEST_CASE("a recover handler returning Result can also succeed implicitly") {
  auto tree = beet::node(small).recover([](TooBig t) -> Result<int, Rewritten> {
    if (t.value < 1000) return 100;
    return beet::make_unexpected(Rewritten{"way too big"});
  });
  CHECK(run(tree, 500).value() == 100);
  CHECK(run(tree, 5000).error().why == "way too big");
}

TEST_CASE("a recover handler can be a coroutine") {
  auto tree = beet::node(non_negative).recover([](Negative) -> Task<Result<int, never>> {
    co_await beet::running;
    co_return -1;
  });
  beet::Runner r{tree, -5};
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Success);
  CHECK(r.result().value() == -1);
}

TEST_CASE("fallback tries the next alternative on the same input") {
  auto tree = beet::node(small).fallback([](int x) -> Result<int, Negative> {
    if (x < 0) return beet::make_unexpected(Negative{});
    return 100;
  });
  static_assert(std::is_same_v<output_t<decltype(tree)>, int>);
  static_assert(std::is_same_v<error_t<decltype(tree)>, Negative>);
  CHECK(run(tree, 5).value() == 5);
  CHECK(run(tree, 500).value() == 100);
}

TEST_CASE("fallback with differing outputs yields a variant") {
  auto tree = beet::fallback(small, show);
  static_assert(std::is_same_v<output_t<decltype(tree)>, std::variant<int, std::string>>);
  static_assert(std::is_same_v<error_t<decltype(tree)>, never>);
  CHECK(std::get<int>(run(tree, 5).value()) == 5);
  CHECK(std::get<std::string>(run(tree, 500).value()) == "500");
}

TEST_CASE("finally runs on success, failure, and halt") {
  int calls = 0;
  auto tree = beet::node(non_negative).then(slow_add_one).finally([&calls] { ++calls; });

  CHECK(run(tree, 1).value() == 2);
  CHECK(calls == 1);
  CHECK_FALSE(run(tree, -1).has_value());
  CHECK(calls == 2);

  beet::Runner r{tree, 1};
  CHECK(r.tick() == Status::Running);
  r.halt();
  CHECK(calls == 3);
}

TEST_CASE("coroutine nodes can await composed subtrees directly") {
  auto validated = beet::node(non_negative).then(small);
  auto tree = beet::node([&validated](std::vector<int> xs) -> Task<Result<int, std::variant<Negative, TooBig>>> {
    int total = 0;
    for (int x : xs) total += co_await validated(x);
    co_return total;
  });
  CHECK(run(tree, {1, 2, 3}).value() == 6);
  CHECK(std::holds_alternative<TooBig>(run(tree, {1, 200, -3}).error()));
}
