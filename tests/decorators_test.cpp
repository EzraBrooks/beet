#include <doctest/doctest.h>

#include <stdexcept>
#include <variant>

#include "beet/beet.hpp"

namespace {

using beet::error_t;
using beet::input_t;
using beet::never;
using beet::Result;
using beet::Status;
using beet::Task;

struct Flaky {};

Task<Result<int, never>> takes_ticks(int n) {
  for (int i = 1; i < n; ++i) co_await beet::running;
  co_return n;
}

}  // namespace

TEST_CASE("retry re-runs a failing node up to the attempt limit") {
  int calls = 0;
  auto flaky = beet::node([&calls](int succeed_on) -> Result<int, Flaky> {
    if (++calls < succeed_on) return beet::make_unexpected(Flaky{});
    return calls;
  });

  beet::Runner ok{beet::retry(3, flaky), 3};
  ok.tick();
  CHECK(ok.result().value() == 3);

  calls = 0;
  beet::Runner fails{beet::retry(2, flaky), 3};
  fails.tick();
  CHECK_FALSE(fails.result().has_value());
  CHECK(calls == 2);

  CHECK_THROWS_AS(beet::retry(0, flaky), std::invalid_argument);
}

TEST_CASE("repeat runs a node several times and stops at the first failure") {
  int calls = 0;
  auto counter = beet::node([&calls](int limit) -> Result<int, Flaky> {
    if (++calls > limit) return beet::make_unexpected(Flaky{});
    return calls;
  });

  beet::Runner ok{beet::repeat(3, counter), 10};
  ok.tick();
  CHECK(ok.result().value() == 3);

  calls = 0;
  beet::Runner fails{beet::repeat(5, counter), 2};
  fails.tick();
  CHECK_FALSE(fails.result().has_value());
  CHECK(calls == 3);
}

TEST_CASE("timeout_ticks fails with Timeout and adds it to the error set") {
  auto tree = beet::timeout_ticks(3, takes_ticks);
  static_assert(std::is_same_v<error_t<decltype(tree)>, beet::Timeout>);

  beet::Runner fast{tree, 3};
  CHECK(fast.tick() == Status::Running);
  CHECK(fast.tick() == Status::Running);
  CHECK(fast.tick() == Status::Success);

  beet::Runner slow{tree, 4};
  CHECK(slow.tick() == Status::Running);
  CHECK(slow.tick() == Status::Running);
  CHECK(slow.tick() == Status::Failure);
  CHECK(slow.result().error() == beet::Timeout{3});
}

TEST_CASE("condition passes its input through or fails") {
  auto positive = beet::condition([](int x) { return x > 0; });
  static_assert(std::is_same_v<input_t<decltype(positive)>, int>);
  static_assert(std::is_same_v<error_t<decltype(positive)>, beet::ConditionFailed>);

  auto tree = positive.then([](int x) { return x * 10; });
  beet::Runner yes{tree, 4};
  yes.tick();
  CHECK(yes.result().value() == 40);

  beet::Runner no{tree, -4};
  no.tick();
  CHECK(no.result().error() == beet::ConditionFailed{});
}
