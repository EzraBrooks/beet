#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <variant>

#include "beet/beet.hpp"

namespace {

using beet::error_t;
using beet::input_t;
using beet::never;
using beet::output_t;
using beet::Result;
using beet::Status;
using beet::Task;
using beet::unit;

struct Bad {};
struct Worse {};

int twice(int x) { return x * 2; }
Result<std::string, Bad> named(int x) {
  if (x < 0) return beet::make_unexpected(Bad{});
  return std::to_string(x);
}
Task<Result<double, Worse>> halve(int x) { co_return x / 2.0; }

auto n_plain = beet::node(twice);
auto n_result = beet::node(named);
auto n_task = beet::node(halve);
auto n_void = beet::node([] {});
auto n_generic = beet::node<int>([](auto x) { return x + 1; });
auto n_function = beet::node(std::function<int(const int&)>(twice));

static_assert(std::is_same_v<input_t<decltype(n_plain)>, int>);
static_assert(std::is_same_v<output_t<decltype(n_plain)>, int>);
static_assert(std::is_same_v<error_t<decltype(n_plain)>, never>);
static_assert(std::is_same_v<output_t<decltype(n_result)>, std::string>);
static_assert(std::is_same_v<error_t<decltype(n_result)>, Bad>);
static_assert(std::is_same_v<output_t<decltype(n_task)>, double>);
static_assert(std::is_same_v<error_t<decltype(n_task)>, Worse>);
static_assert(std::is_same_v<input_t<decltype(n_void)>, unit>);
static_assert(std::is_same_v<output_t<decltype(n_void)>, unit>);
static_assert(std::is_same_v<output_t<decltype(n_generic)>, int>);
static_assert(std::is_same_v<input_t<decltype(n_function)>, int>);

template <class N>
auto run_once(const N& n, input_t<N> in) {
  beet::Runner r{n, std::move(in)};
  REQUIRE(r.tick() != Status::Running);
  return r.result();
}

}  // namespace

TEST_CASE("lifted callables run when ticked") {
  CHECK(run_once(n_plain, 4).value() == 8);
  CHECK(run_once(n_result, 7).value() == "7");
  CHECK_FALSE(run_once(n_result, -1).has_value());
  CHECK(run_once(n_task, 3).value() == doctest::Approx(1.5));
  CHECK(run_once(n_generic, 1).value() == 2);
  CHECK(run_once(n_function, 5).value() == 10);
}

TEST_CASE("plain callables are deferred until the first tick") {
  int calls = 0;
  beet::Runner r{beet::node([&calls] { ++calls; })};
  CHECK(calls == 0);
  r.tick();
  CHECK(calls == 1);
}

TEST_CASE("AnyNode erases a subtree's type and can widen its error set") {
  beet::AnyNode<int, std::string, Bad> exact = beet::node(named);
  CHECK(run_once(exact, 3).value() == "3");

  beet::AnyNode<int, std::string, std::variant<Bad, Worse>> wider = beet::node(named);
  auto failed = run_once(wider, -3);
  REQUIRE_FALSE(failed.has_value());
  CHECK(std::holds_alternative<Bad>(failed.error()));

  beet::AnyNode<int, int, Bad> chained = beet::node(twice).then(twice);
  CHECK(run_once(chained, 1).value() == 4);
}
