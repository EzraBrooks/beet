#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

#include "beet/beet.hpp"

namespace {

using beet::Result;
using beet::Status;
using beet::Task;

struct Oops {
  std::string why;
};

Task<Result<int, beet::never>> count_ticks(int ticks) {
  for (int i = 1; i < ticks; ++i) {
    co_await beet::running;
  }
  co_return ticks;
}

Task<Result<int, Oops>> fail_after(int ticks) {
  for (int i = 1; i < ticks; ++i) {
    co_await beet::running;
  }
  co_return beet::make_unexpected(Oops{"late"});
}

Task<Result<int, Oops>> sum_children() noexcept {
  int counted = co_await count_ticks(2);
  int failed = co_await fail_after(2);
  co_return counted + failed + 1000;
}

struct Flag {
  bool* destroyed;
  ~Flag() { *destroyed = true; }
};

Task<Result<int, beet::never>> forever(bool* destroyed) {
  Flag flag{destroyed};
  for (;;) {
    co_await beet::running;
  }
}

Task<Result<int, Oops>> throws() noexcept {
  co_await beet::running;
  throw std::runtime_error("boom");
}

}  // namespace

TEST_CASE("a coroutine node suspends on running and resumes on the next tick") {
  beet::Runner runner{beet::node(count_ticks), 3};
  CHECK(runner.status() == Status::Idle);
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Success);
  CHECK(runner.result().value() == 3);
  CHECK(runner.tick() == Status::Success);
}

TEST_CASE(
    "co_await on a failing child propagates its error without resuming the "
    "parent") {
  beet::Runner runner{beet::node(sum_children)};
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Failure);
  CHECK(runner.result().error().why == "late");
}

TEST_CASE(
    "halting destroys suspended frames, and the next tick restarts the tree") {
  bool destroyed = false;
  beet::Runner runner{beet::node(forever), &destroyed};
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Running);
  CHECK_FALSE(destroyed);
  runner.halt();
  CHECK(destroyed);
  CHECK(runner.status() == Status::Idle);
  destroyed = false;
  CHECK(runner.tick() == Status::Running);
  CHECK_FALSE(destroyed);
}

TEST_CASE(
    "exceptions thrown by a noexcept coroutine node surface from tick()") {
  beet::Runner runner{beet::node(throws)};
  CHECK(runner.tick() == Status::Running);
  CHECK_THROWS_AS(runner.tick(), std::runtime_error);
}

TEST_CASE("result() before completion is a logic error") {
  beet::Runner runner{beet::node(count_ticks), 2};
  CHECK_THROWS_AS((void)runner.result(), std::logic_error);
  runner.tick();
  CHECK_THROWS_AS((void)runner.result(), std::logic_error);
}

TEST_CASE("reset() swaps the input for the next run") {
  beet::Runner runner{beet::node(count_ticks), 1};
  CHECK(runner.tick() == Status::Success);
  runner.reset(2);
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Success);
  CHECK(runner.result().value() == 2);
}
