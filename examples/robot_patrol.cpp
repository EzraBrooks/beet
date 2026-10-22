#include <chrono>
#include <cstdio>

#include "robot_mission.hpp"

int main() {
  using namespace std::chrono_literals;

  // Ten ticks per period. `run()` defaults to a one-second period.
  const auto tree = beet::rate<10>(mission::make_mission());
  // Runs the mission ten times faster than real time, which is handy for
  // tests.
  constexpr auto period = 100ms;

  for (mission::Pose goal : mission::goals) {
    std::printf("goal (%.0f, %.0f):\n", goal.x, goal.y);
    beet::Runner runner{tree, goal};
    runner.run(period);
    std::printf("  %s\n", runner.result()->text.c_str());
  }
}
