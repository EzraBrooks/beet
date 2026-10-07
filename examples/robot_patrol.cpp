#include <cstdio>

#include "robot_mission.hpp"

int main() {
  const auto tree = mission::make_mission();

  for (mission::Pose goal : mission::goals) {
    std::printf("goal (%.0f, %.0f):\n", goal.x, goal.y);
    beet::Runner runner{tree, goal};
    int ticks = 1;
    while (runner.tick() == beet::Status::Running) ++ticks;
    std::printf("  %s (%d ticks)\n", runner.result()->text.c_str(), ticks);
  }
}
