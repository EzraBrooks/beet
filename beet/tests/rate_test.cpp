#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stop_token>
#include <utility>
#include <vector>

#include "beet/beet.hpp"
#include "beet/observe.hpp"

namespace {

using beet::never;
using beet::Result;
using beet::Status;
using beet::Task;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

int twice(int value) noexcept { return value * 2; }

// Records `now` each time it is ticked, and finishes on its `steps`th tick.
auto recorder(std::vector<int>& seen, const int& now, int steps) {
  return beet::node(
      [&seen, &now, steps]() noexcept -> Task<Result<int, never>> {
        for (int i = 1;; ++i) {
          seen.push_back(now);
          if (i == steps) {
            co_return steps;
          }
          co_await beet::running;
        }
      });
}

template <class Tree>
int tick_until_done(beet::Runner<Tree>& runner, int& now) {
  for (now = 0; runner.tick() == Status::Running; ++now) {
  }
  return now;
}

static_assert(beet::rate_v<decltype(beet::node(twice))> == 0);
static_assert(beet::rate_v<decltype(beet::rate<3>(twice))> == 3);
static_assert(beet::rate_v<decltype(beet::named<"x">(beet::rate<3>(twice)))> ==
              3);
static_assert(beet::rate_v<decltype(beet::rate<6>(beet::rate<3>(twice)))> == 6);
static_assert(beet::rate_v<decltype(beet::sequence(beet::rate<2>(twice),
                                                   beet::rate<3>(twice)))> ==
              6);
static_assert(decltype(beet::rate<5>(twice))::is_sync);

struct FakePacer {
  std::chrono::steady_clock::time_point time;

  [[nodiscard]] std::chrono::steady_clock::time_point now() const {
    return time;
  }
  void sleep_until(std::chrono::steady_clock::time_point deadline,
                   const std::stop_token& /*unused*/) {
    time = std::max(time, deadline);
  }
};

// Records the fake time of each tick, then spends `costs[i]` of fake time on
// tick `i`. Finishes after its last cost.
auto timed(FakePacer& pacer, std::vector<nanoseconds>& at,
           std::vector<nanoseconds> costs) {
  const auto start = pacer.time;
  return beet::node(
      [&pacer, &at, start,
       costs = std::move(costs)]() noexcept -> Task<Result<int, never>> {
        for (std::size_t i = 0;; ++i) {
          at.push_back(pacer.time - start);
          pacer.time += costs[i];
          if (i + 1 == costs.size()) {
            co_return 0;
          }
          co_await beet::running;
        }
      });
}

std::vector<nanoseconds> ms(std::initializer_list<int> values) {
  std::vector<nanoseconds> out;
  for (int value : values) {
    out.emplace_back(milliseconds(value));
  }
  return out;
}

}  // namespace

TEST_CASE("a rate region ticks its child once every parent / K ticks") {
  int now = 0;
  std::vector<int> fast;
  std::vector<int> slow;
  beet::Runner runner{beet::rate<4>(beet::parallel_all(
      recorder(fast, now, 9), beet::rate<1>(recorder(slow, now, 3))))};
  static_assert(decltype(runner)::rate == 4);

  CHECK(tick_until_done(runner, now) == 8);
  CHECK(fast == std::vector{0, 1, 2, 3, 4, 5, 6, 7, 8});
  CHECK(slow == std::vector{0, 4, 8});
}

TEST_CASE("the root ticks at the least common multiple of sibling rates") {
  int now = 0;
  std::vector<int> fours;
  std::vector<int> sixes;
  beet::Runner runner{
      beet::parallel_all(beet::rate<4>(recorder(fours, now, 3)),
                         beet::rate<6>(recorder(sixes, now, 3)))};
  static_assert(decltype(runner)::rate == 12);

  CHECK(tick_until_done(runner, now) == 6);
  CHECK(fours == std::vector{0, 3, 6});
  CHECK(sixes == std::vector{0, 2, 4});
}

TEST_CASE("a region reached mid-run ticks right away, then on its own rate") {
  int now = 0;
  std::vector<int> before;
  std::vector<int> region;
  beet::Runner runner{beet::rate<4>(beet::sequence(
      recorder(before, now, 3), beet::rate<2>(recorder(region, now, 3))))};

  CHECK(tick_until_done(runner, now) == 6);
  CHECK(before == std::vector{0, 1, 2});
  CHECK(region == std::vector{2, 4, 6});
}

TEST_CASE("nested regions divide their enclosing region's ticks") {
  int now = 0;
  std::vector<int> seen;
  beet::Runner runner{
      beet::rate<8>(beet::rate<4>(beet::rate<1>(recorder(seen, now, 3))))};

  CHECK(tick_until_done(runner, now) == 16);
  CHECK(seen == std::vector{0, 8, 16});
}

TEST_CASE("a plain function under a rate finishes on the first tick") {
  beet::Runner runner{beet::rate<5>(twice), 3};
  CHECK(runner.tick() == Status::Success);
  CHECK(runner.result().value() == 6);
}

TEST_CASE("tick-based decorators count the ticks of their region") {
  int now = 0;
  std::vector<int> seen;
  beet::Runner runner{beet::rate<4>(
      beet::rate<1>(beet::timeout_ticks<2>(recorder(seen, now, 10))))};

  CHECK(tick_until_done(runner, now) == 4);
  CHECK(runner.status() == Status::Failure);
  CHECK(seen == std::vector{0, 4});
}

TEST_CASE("listeners see rate nodes") {
  int now = 0;
  std::vector<int> seen;
  auto tree = beet::rate<2>(beet::rate<1>(recorder(seen, now, 2)));
  using Tree = decltype(tree);
  static_assert(beet::tree_info<Tree>[0].kind == "rate");
  static_assert(beet::tree_info<Tree>[1].kind == "rate");
  static_assert(beet::tree_info<Tree>[2].kind == "leaf");

  beet::StatusTable<Tree> table;
  beet::Runner runner{tree, beet::unit{}, table.listeners()};
  CHECK(runner.tick() == Status::Running);
  CHECK(table.status(2) == beet::NodeStatus::Running);
  CHECK(runner.tick() == Status::Running);
  CHECK(runner.tick() == Status::Success);
  CHECK(table.status(0) == beet::NodeStatus::Success);
  CHECK(table.status(2) == beet::NodeStatus::Success);
}

TEST_CASE("run() spreads ticks evenly over the period without drift") {
  FakePacer pacer;
  std::vector<nanoseconds> at;
  beet::Runner runner{
      beet::rate<600>(timed(pacer, at, std::vector<nanoseconds>(1201)))};

  CHECK(runner.run(milliseconds(100), {}, pacer) == Status::Success);
  REQUIRE(at.size() == 1201);
  for (std::size_t i = 0; i < at.size(); ++i) {
    CHECK(at[i] ==
          nanoseconds(static_cast<std::int64_t>(i) * 100'000'000 / 600));
  }
  CHECK(at[600] == milliseconds(100));
  CHECK(at[1200] == milliseconds(200));
}

TEST_CASE("run() truncates uneven tick intervals without drift") {
  FakePacer pacer;
  std::vector<nanoseconds> at;
  beet::Runner runner{
      beet::rate<3>(timed(pacer, at, std::vector<nanoseconds>(4)))};

  CHECK(runner.run(std::chrono::seconds{1}, {}, pacer) == Status::Success);
  CHECK(at == std::vector<nanoseconds>{nanoseconds(0), nanoseconds(333333333),
                                       nanoseconds(666666666),
                                       std::chrono::seconds{1}});
}

TEST_CASE("run() returns Running once stop is requested") {
  FakePacer pacer;
  std::stop_source stop;
  int ticks = 0;
  beet::Runner runner{
      beet::rate<10>([&]() noexcept -> Task<Result<int, never>> {
        for (;;) {
          if (++ticks == 3) {
            stop.request_stop();
          }
          co_await beet::running;
        }
      })};

  CHECK(runner.run(std::chrono::seconds{1}, {.stop = stop.get_token()},
                   pacer) == Status::Running);
  CHECK(ticks == 3);
  CHECK(runner.status() == Status::Running);
}

TEST_CASE("run() reports overruns and handles missed ticks like Tokio") {
  // Ten ticks per second, so ticks are due every 100ms. Tick 1 starts at
  // 100ms and ends at 350ms, past the deadlines at 200ms and 300ms.
  const auto costs = ms({0, 250, 0, 0, 0});

  auto run = [&](beet::MissedTick missed_tick) {
    FakePacer pacer;
    std::vector<nanoseconds> at;
    std::vector<beet::Overrun> overruns;
    beet::Runner runner{beet::rate<10>(timed(pacer, at, costs))};
    beet::RunOptions options{.missed_tick = missed_tick,
                             .on_overrun = [&](const beet::Overrun& overrun) {
                               overruns.push_back(overrun);
                             }};
    CHECK(runner.run(std::chrono::seconds{1}, options, pacer) ==
          Status::Success);
    REQUIRE(overruns.size() == 1);
    CHECK(overruns[0].tick == 1);
    CHECK(overruns[0].late == milliseconds(150));
    CHECK(overruns[0].missed == 1);
    CHECK(overruns[0].interval == milliseconds(100));
    return at;
  };

  SUBCASE("Burst catches up back to back") {
    CHECK(run(beet::MissedTick::Burst) == ms({0, 100, 350, 350, 400}));
  }
  SUBCASE("Delay restarts the schedule from the late tick") {
    CHECK(run(beet::MissedTick::Delay) == ms({0, 100, 350, 450, 550}));
  }
  SUBCASE("Skip drops passed deadlines and keeps the schedule") {
    CHECK(run(beet::MissedTick::Skip) == ms({0, 100, 350, 400, 500}));
  }
}

TEST_CASE("OverrunWarning writes at most one line per second to std::clog") {
  std::ostringstream captured;
  std::streambuf* original = std::clog.rdbuf(captured.rdbuf());
  beet::OverrunWarning warn;
  warn(beet::Overrun{.tick = 1,
                     .late = std::chrono::microseconds(912),
                     .missed = 2,
                     .interval = nanoseconds(1666667)});
  warn(beet::Overrun{.tick = 2,
                     .late = milliseconds(5),
                     .missed = 1,
                     .interval = nanoseconds(1666667)});
  std::clog.rdbuf(original);

  CHECK(captured.str() ==
        "beet: 1 tick(s) overran the 1.667ms tick interval (worst +0.912ms, 2 "
        "further deadline(s) missed)\n");
}

TEST_CASE("next_tick computes the schedule from the time alone") {
  using time_point = std::chrono::steady_clock::time_point;
  const time_point start{};
  const beet::detail::schedule<time_point> sched{
      .period = std::chrono::seconds{1},
      .ticks_per_period = 10,
      .anchor = start};

  const auto on_time = beet::detail::next_tick(
      sched, 0, start + milliseconds(40), beet::MissedTick::Skip);
  CHECK_FALSE(on_time.late);
  CHECK_FALSE(on_time.overrun);
  CHECK(on_time.next.deadline(on_time.next.slot) == start + milliseconds(100));

  // Tick 1 ends at 350ms, past the deadlines at 200ms and 300ms.
  const auto late = beet::detail::next_tick(
      on_time.next, 1, start + milliseconds(350), beet::MissedTick::Skip);
  CHECK(late.late);
  REQUIRE(late.overrun);
  CHECK(late.overrun->late == milliseconds(150));
  CHECK(late.overrun->missed == 1);
  CHECK(late.next.deadline(late.next.slot) == start + milliseconds(300));
}

TEST_CASE("Burst reports a tick that overruns while catching up") {
  // Tick 1 ends at 350ms, one deadline behind. Tick 2 catches up but ends at
  // 650ms, missing the deadlines at 400ms, 500ms and 600ms as well.
  FakePacer pacer;
  std::vector<nanoseconds> at;
  std::vector<beet::Overrun> overruns;
  beet::Runner runner{
      beet::rate<10>(timed(pacer, at, ms({0, 250, 300, 0, 0, 0})))};
  const beet::RunOptions options{
      .missed_tick = beet::MissedTick::Burst,
      .on_overrun = [&](const beet::Overrun& overrun) {
        overruns.push_back(overrun);
      }};

  CHECK(runner.run(std::chrono::seconds{1}, options, pacer) == Status::Success);
  CHECK(at == ms({0, 100, 350, 650, 650, 650}));
  REQUIRE(overruns.size() == 2);
  CHECK(overruns[0].missed == 1);
  CHECK(overruns[1].tick == 2);
  CHECK(overruns[1].late == milliseconds(350));
  CHECK(overruns[1].missed == 3);
}
