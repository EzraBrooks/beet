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

Task<Result<int, beet::never>> count_ticks(int n) {
  for (int i = 1; i < n; ++i) co_await beet::running;
  co_return n;
}

Task<Result<int, Oops>> fail_after(int n) {
  for (int i = 1; i < n; ++i) co_await beet::running;
  co_return beet::make_unexpected(Oops{"late"});
}

Task<Result<int, Oops>> sum_children() {
  int a = co_await count_ticks(2);
  int b = co_await fail_after(2);
  co_return a + b + 1000;
}

struct Flag {
  bool* destroyed;
  ~Flag() { *destroyed = true; }
};

Task<Result<int, beet::never>> forever(bool* destroyed) {
  Flag flag{destroyed};
  for (;;) co_await beet::running;
}

Task<Result<int, Oops>> throws() {
  co_await beet::running;
  throw std::runtime_error("boom");
}

}  // namespace

TEST_CASE("a coroutine node suspends on running and resumes on the next tick") {
  beet::Runner r{beet::node(count_ticks), 3};
  CHECK(r.status() == Status::Idle);
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Success);
  CHECK(r.result().value() == 3);
  CHECK(r.tick() == Status::Success);
}

TEST_CASE(
    "co_await on a failing child propagates its error without resuming the "
    "parent") {
  beet::Runner r{beet::node(sum_children)};
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Failure);
  CHECK(r.result().error().why == "late");
}

TEST_CASE(
    "halting destroys suspended frames, and the next tick restarts the tree") {
  bool destroyed = false;
  beet::Runner r{beet::node(forever), &destroyed};
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Running);
  CHECK_FALSE(destroyed);
  r.halt();
  CHECK(destroyed);
  CHECK(r.status() == Status::Idle);
  destroyed = false;
  CHECK(r.tick() == Status::Running);
  CHECK_FALSE(destroyed);
}

TEST_CASE("exceptions thrown by a node surface from tick()") {
  beet::Runner r{beet::node(throws)};
  CHECK(r.tick() == Status::Running);
  CHECK_THROWS_AS(r.tick(), std::runtime_error);
}

TEST_CASE("result() before completion is a logic error") {
  beet::Runner r{beet::node(count_ticks), 2};
  CHECK_THROWS_AS((void)r.result(), std::logic_error);
  r.tick();
  CHECK_THROWS_AS((void)r.result(), std::logic_error);
}

TEST_CASE("reset() swaps the input for the next run") {
  beet::Runner r{beet::node(count_ticks), 1};
  CHECK(r.tick() == Status::Success);
  r.reset(2);
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Success);
  CHECK(r.result().value() == 2);
}
