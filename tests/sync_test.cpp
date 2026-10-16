#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "beet/beet.hpp"
#include "beet/observe.hpp"

namespace {

using beet::never;
using beet::Result;
using beet::Status;
using beet::Task;

struct Bad {
  friend bool operator==(const Bad&, const Bad&) = default;
};

Result<int, Bad> check(int value) noexcept {
  if (value < 0) {
    return beet::make_unexpected(Bad{});
  }
  return value;
}
int twice(int value) noexcept { return value * 2; }

Task<Result<int, never>> slow(int value) noexcept {
  co_await beet::running;
  co_return value + 1;
}

Task<Result<int, never>> slow_handler(Bad /*unused*/) noexcept {
  co_await beet::running;
  co_return -100;
}

int quick_handler(Bad /*unused*/) noexcept { return -1; }

template <class N>
constexpr bool sync_v = std::remove_cvref_t<N>::is_sync;

static_assert(sync_v<decltype(beet::node(twice))>);
static_assert(!sync_v<decltype(beet::node(slow))>);
static_assert(sync_v<decltype(beet::sequence(check, twice))>);
static_assert(!sync_v<decltype(beet::sequence(check, slow))>);
static_assert(sync_v<decltype(beet::fallback(check, twice))>);
static_assert(!sync_v<decltype(beet::fallback(check, slow))>);
static_assert(sync_v<decltype(beet::recover(check, quick_handler))>);
static_assert(!sync_v<decltype(beet::recover(check, slow_handler))>);
static_assert(!sync_v<decltype(beet::recover(slow, quick_handler))>);
static_assert(sync_v<decltype(beet::finally(check, [] {}))>);
static_assert(sync_v<decltype(beet::named<"n">(check))>);
static_assert(sync_v<decltype(beet::retry<2>(check))>);
static_assert(sync_v<decltype(beet::repeat<2>(check))>);
static_assert(sync_v<decltype(beet::timeout_ticks<1>(check))>);
static_assert(!sync_v<decltype(beet::timeout_ticks<1>(slow))>);
static_assert(!sync_v<decltype(beet::parallel_all(check, twice))>);
static_assert(!sync_v<beet::AnyNode<int, int, Bad>>);

struct Recorder {
  std::vector<std::string> events;

  void on_tick_begin(std::uint64_t tick) {
    events.push_back("tick:" + std::to_string(tick));
  }
  void on_tick_end(std::uint64_t /*unused*/, Status /*unused*/) {}
  void on_start(std::uint32_t id) {
    events.push_back("start:" + std::to_string(id));
  }
  void on_finish(std::uint32_t id, Status status) {
    events.push_back("finish:" + std::to_string(id) +
                     (status == Status::Success ? ":ok" : ":fail"));
  }
  void on_halt(std::uint32_t id) {
    events.push_back("halt:" + std::to_string(id));
  }
};

}  // namespace

TEST_CASE("a sync tree finishes on its first tick and is not re-run") {
  int calls = 0;
  int cleanups = 0;
  auto counted = [&calls](int value) noexcept {
    ++calls;
    return value;
  };
  auto tree = beet::finally(
      beet::recover(beet::sequence(check, counted, twice), quick_handler),
      [&cleanups] { ++cleanups; });
  static_assert(sync_v<decltype(tree)>);

  beet::Runner runner{tree, 4};
  CHECK(runner.status() == Status::Idle);
  CHECK(runner.tick() == Status::Success);
  CHECK(runner.result().value() == 8);
  CHECK(runner.tick() == Status::Success);
  CHECK(calls == 1);
  CHECK(cleanups == 1);

  runner.reset(-4);
  CHECK(runner.tick() == Status::Success);
  CHECK(runner.result().value() == -1);
  CHECK(calls == 1);
  CHECK(cleanups == 2);
}

TEST_CASE("a sync child's failure completes its async parent") {
  int started = 0;
  auto counted_slow =
      [&started](int value) noexcept -> Task<Result<int, never>> {
    ++started;
    co_await beet::running;
    co_return value;
  };
  auto tree = beet::sequence(check, counted_slow);
  static_assert(!sync_v<decltype(tree)>);

  beet::Runner fails{tree, -1};
  CHECK(fails.tick() == Status::Failure);
  CHECK(fails.result().error() == Bad{});
  CHECK(started == 0);

  beet::Runner passes{tree, 3};
  CHECK(passes.tick() == Status::Running);
  CHECK(passes.tick() == Status::Success);
  CHECK(passes.result().value() == 3);
  CHECK(started == 1);
}

TEST_CASE("an async fallback tries the next child after a sync failure") {
  auto tree = beet::fallback(check, slow);
  beet::Runner runner{tree, -5};
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Success);
  CHECK(runner.result().value() == -4);
}

TEST_CASE("recover runs sync and async handlers") {
  beet::Runner quick{beet::recover(beet::sequence(slow, check), quick_handler),
                     -5};
  CHECK(quick.tick() == Status::Running);
  CHECK(quick.tick() == Status::Success);
  CHECK(quick.result().value() == -1);

  beet::Runner async_handler{beet::recover(check, slow_handler), -5};
  CHECK(async_handler.tick() == Status::Running);
  CHECK(async_handler.tick() == Status::Success);
  CHECK(async_handler.result().value() == -100);
}

TEST_CASE("a traced sync tree reports events in depth-first order") {
  // IDs: 0 recover, 1 sequence, 2 check, 3 twice
  auto tree = beet::recover(beet::sequence(check, twice), quick_handler);
  Recorder ok;
  beet::Runner passes{tree, 2, ok};
  CHECK(passes.tick() == Status::Success);
  CHECK(ok.events == std::vector<std::string>{"tick:0", "start:0", "start:1",
                                              "start:2", "finish:2:ok",
                                              "start:3", "finish:3:ok",
                                              "finish:1:ok", "finish:0:ok"});

  Recorder failed;
  beet::Runner fails{tree, -2, failed};
  CHECK(fails.tick() == Status::Success);
  CHECK(failed.events == std::vector<std::string>{
                             "tick:0", "start:0", "start:1", "start:2",
                             "finish:2:fail", "finish:1:fail", "finish:0:ok"});
}
