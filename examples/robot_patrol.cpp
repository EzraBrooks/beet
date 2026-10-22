#include <chrono>
#include <format>
#include <iostream>

#include "beet/beet.hpp"
#include "robot_mission.hpp"

int main() {
  using namespace std::chrono_literals;

  // Ten ticks per period. `run()` defaults to a one-second period.
  const auto tree = beet::rate<10>(mission::make_mission());
  // Runs the mission ten times faster than real time, which is handy for
  // tests.
  constexpr auto period = 100ms;  // NOLINT(misc-include-cleaner)

  for (mission::Pose goal : mission::goals) {
    std::cout << std::format("goal ({:.0f}, {:.0f}):\n", goal.x, goal.y);
    beet::Runner runner{tree, goal};
    runner.run(period);
    std::cout << std::format("  {}\n", runner.result()->text);
  }
}
