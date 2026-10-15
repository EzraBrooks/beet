#include <doctest/doctest.h>

#include <vector>

#include "beet/beet.hpp"
#include "beet/observe.hpp"

namespace {

using beet::never;
using beet::NodeStatus;
using beet::Result;
using beet::Status;
using beet::Task;

struct Bad {};

template <class R>
void tick_through(R& runner, int& tick, int last) {
  for (; tick <= last; ++tick) runner.tick();
}

}  // namespace

TEST_CASE("a nested rate ticks its subtree every P / Hz ticks of its region") {
  int tick = 0;
  int fast_ticks = 0;
  std::vector<int> probed;
  auto fast = [&fast_ticks]() noexcept -> Task<Result<never, never>> {
    for (;;) {
      ++fast_ticks;
      co_await beet::running;
    }
  };
  auto probe = [&]() noexcept { probed.push_back(tick); };

  auto tree = beet::rate<600>(
      beet::parallel_any(fast, beet::rate<1>(beet::repeat(probe))));
  beet::Runner runner{tree};
  static_assert(decltype(runner)::tick_hz == 600);

  tick_through(runner, tick, 1200);
  CHECK(probed == std::vector{0, 600, 1200});
  CHECK(fast_ticks == 1201);
}

TEST_CASE("nested rate ratios multiply") {
  int tick = 0;
  std::vector<int> probed;
  auto probe = [&]() noexcept { probed.push_back(tick); };
  auto tree =
      beet::rate<600>(beet::rate<100>(beet::rate<50>(beet::repeat(probe))));
  beet::Runner runner{tree};

  tick_through(runner, tick, 30);
  CHECK(probed == std::vector{0, 12, 24});
}

TEST_CASE("the outermost rate ticks every tick and sets tick_hz") {
  int calls = 0;
  beet::Runner lone{
      beet::rate<1>(beet::repeat([&calls]() noexcept { ++calls; }))};
  static_assert(decltype(lone)::tick_hz == 1);
  for (int i = 0; i < 5; ++i) lone.tick();
  CHECK(calls == 5);

  beet::Runner plain{beet::node([]() noexcept {})};
  static_assert(decltype(plain)::tick_hz == 0);
}

TEST_CASE(
    "repeat without a count reruns its child once per tick until it fails") {
  int calls = 0;
  auto tree = beet::repeat([&calls]() noexcept -> Result<int, Bad> {
    if (++calls == 3) return beet::make_unexpected(Bad{});
    return calls;
  });
  static_assert(std::is_same_v<beet::output_t<decltype(tree)>, never>);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, Bad>);

  beet::Runner runner{tree};
  CHECK(runner.tick() == Status::Running);
  CHECK(calls == 1);
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Failure);
  CHECK(calls == 3);
}

TEST_CASE("halting a rate region halts its subtree") {
  auto tree = beet::rate<10>(beet::rate<5>(beet::repeat([]() noexcept {})));
  using Tree = decltype(tree);
  constexpr auto info = beet::describe<Tree>();
  static_assert(info[0].kind == "rate" && info[0].hz == 10);
  static_assert(info[1].kind == "rate" && info[1].hz == 5);
  static_assert(info[2].kind == "repeat" && info[2].hz == 0);

  beet::StatusTable<Tree> table;
  beet::Runner runner{tree, beet::unit{}, table};
  runner.tick();
  runner.tick();
  CHECK(table.status(1) == NodeStatus::Running);
  runner.halt();
  CHECK(table.status(0) == NodeStatus::Halted);
  CHECK(table.status(1) == NodeStatus::Halted);
  CHECK(table.status(2) == NodeStatus::Halted);
}
