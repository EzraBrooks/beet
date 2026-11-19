#include <doctest/doctest.h>

#include <type_traits>

#include "beet/beet.hpp"

namespace {

using beet::input_t;
using beet::never;
using beet::Result;
using beet::Status;
using beet::Task;

struct Flaky {};

Task<Result<int, never>> takes_ticks(int ticks) noexcept {
  for (int i = 1; i < ticks; ++i) {
    co_await beet::running;
  }
  co_return ticks;
}

}  // namespace

TEST_CASE(
    "retry re-runs a failing node once per tick up to the attempt limit") {
  int calls = 0;
  auto flaky = beet::node([&calls](int succeed_on) -> Result<int, Flaky> {
    if (++calls < succeed_on) {
      return beet::make_unexpected(Flaky{});
    }
    return calls;
  });

  beet::Runner ok{beet::retry<3>(flaky), 3};
  CHECK(ok.tick() == Status::Running);
  CHECK(calls == 1);
  CHECK(ok.tick() == Status::Running);
  CHECK(calls == 2);
  CHECK(ok.tick() == Status::Success);
  CHECK(ok.result().value() == 3);

  calls = 0;
  beet::Runner fails{beet::retry<2>(flaky), 3};
  CHECK(fails.tick() == Status::Running);
  CHECK(fails.tick() == Status::Failure);
  CHECK(calls == 2);

  calls = 0;
  beet::Runner first_try{beet::retry<5>(flaky), 1};
  CHECK(first_try.tick() == Status::Success);
  CHECK(calls == 1);
}

TEST_CASE("repeat runs a node once per tick and stops at the first failure") {
  int calls = 0;
  auto counter = beet::node([&calls](int limit) -> Result<int, Flaky> {
    if (++calls > limit) {
      return beet::make_unexpected(Flaky{});
    }
    return calls;
  });

  beet::Runner ok{beet::repeat<3>(counter), 10};
  CHECK(ok.tick() == Status::Running);
  CHECK(calls == 1);
  CHECK(ok.tick() == Status::Running);
  CHECK(calls == 2);
  CHECK(ok.tick() == Status::Success);
  CHECK(ok.result().value() == 3);

  calls = 0;
  beet::Runner fails{beet::repeat<5>(counter), 2};
  CHECK(fails.tick() == Status::Running);
  CHECK(fails.tick() == Status::Running);
  CHECK(fails.tick() == Status::Failure);
  CHECK(calls == 3);
}

TEST_CASE(
    "repeat starts the next run on the tick after a long child finishes") {
  beet::Runner runner{beet::repeat<2>(takes_ticks), 2};
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Success);
}

TEST_CASE("timeout_ticks fails with Timeout and adds it to the error set") {
  auto tree = beet::timeout_ticks<3>(takes_ticks);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, beet::Timeout>);

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
  auto positive = beet::condition([](int value) noexcept { return value > 0; });
  static_assert(std::is_same_v<input_t<decltype(positive)>, int>);
  static_assert(
      std::is_same_v<beet::error_t<decltype(positive)>, beet::ConditionFailed>);

  auto tree =
      beet::sequence(positive, [](int value) noexcept { return value * 10; });
  beet::Runner yes{tree, 4};
  yes.tick();
  CHECK(yes.result().value() == 40);

  beet::Runner no{tree, -4};
  no.tick();
  CHECK(no.result().error() == beet::ConditionFailed{});
}
