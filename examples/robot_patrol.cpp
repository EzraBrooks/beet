// A robot plans a path to a goal, then drives it while watching its battery.
// Every connection between nodes is a typed value; there is no blackboard.

#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include "beet/beet.hpp"

using beet::Result;
using beet::Task;

struct Pose {
  double x = 0;
  double y = 0;
};
struct Path {
  std::vector<Pose> waypoints;
};
struct Odom {
  double distance = 0;
};
struct Level {
  int percent = 0;
};
struct Summary {
  std::string text;
};

struct PlanError {
  std::string why;
};
struct Stuck {
  Pose where;
};
struct LowBattery {
  int percent;
};

std::string describe(const PlanError& e) { return "planning failed: " + e.why; }
std::string describe(const Stuck& e) {
  return "stuck at (" + std::to_string(e.where.x) + ", " + std::to_string(e.where.y) + ")";
}

Result<Path, PlanError> plan_path(Pose goal) {
  if (goal.x < 0) return beet::make_unexpected(PlanError{"goal is outside the map"});
  Path path;
  for (int i = 1; i <= 4; ++i) path.waypoints.push_back({goal.x * i / 4, goal.y * i / 4});
  return path;
}

Task<Result<Odom, Stuck>> drive(Path path) {
  Odom odom;
  Pose at;
  for (const Pose& next : path.waypoints) {
    co_await beet::running;
    if (next.y > 50) co_return beet::make_unexpected(Stuck{at});
    odom.distance += std::hypot(next.x - at.x, next.y - at.y);
    at = next;
    std::printf("  drove to (%.1f, %.1f)\n", at.x, at.y);
  }
  co_return odom;
}

// Each leg drains the battery in proportion to its length. Fails as soon as the charge drops below 30%.
Task<Result<Level, LowBattery>> watch_battery(Path path) {
  int percent = 70;
  Pose at;
  for (const Pose& next : path.waypoints) {
    co_await beet::running;
    percent -= 2 + static_cast<int>(std::hypot(next.x - at.x, next.y - at.y) / 2);
    at = next;
    if (percent < 30) co_return beet::make_unexpected(LowBattery{percent});
  }
  co_return Level{percent};
}

Summary report(std::tuple<Odom, Level> done) {
  auto [odom, level] = done;
  return {"arrived after " + std::to_string(odom.distance) + "m with " + std::to_string(level.percent) + "% battery"};
}

int main() {
  auto mission = beet::node(plan_path)
                     .then(beet::parallel_all(drive, watch_battery))
                     .then(report)
                     .recover<LowBattery>([](LowBattery b) {
                       return Summary{"returned to dock at " + std::to_string(b.percent) + "% battery"};
                     })
                     .recover([](const auto& e) { return Summary{describe(e)}; });

  static_assert(std::is_same_v<beet::error_t<decltype(mission)>, beet::never>,
                "every failure is handled, so the mission cannot fail");

  for (Pose goal : {Pose{8, 6}, Pose{-1, 0}, Pose{4, 80}, Pose{80, 0}}) {
    std::printf("goal (%.0f, %.0f):\n", goal.x, goal.y);
    beet::Runner runner{mission, goal};
    int ticks = 1;
    while (runner.tick() == beet::Status::Running) ++ticks;
    std::printf("  %s (%d ticks)\n", runner.result()->text.c_str(), ticks);
  }
}
