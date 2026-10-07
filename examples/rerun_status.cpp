// Logs the robot mission's node statuses to Rerun as a graph, one frame per
// tick.
//
//   pixi run -e rerun rerun-example                 # spawns the viewer
//   ./rerun_status --save mission.rrd               # writes a recording
//   instead

#include <cstdint>
#include <string>
#include <string_view>

#include <rerun.hpp>

#include "rerun_tree.hpp"
#include "robot_mission.hpp"

int main(int argc, char** argv) {
  const auto tree = mission::make_mission();
  using Tree = std::remove_cvref_t<decltype(tree)>;

  const rerun::RecordingStream rec("beet_robot_mission");
  if (argc == 3 && std::string_view(argv[1]) == "--save") {
    rec.save(argv[2]).exit_on_failure();
  } else {
    rec.spawn().exit_on_failure();
  }

  const rerun_tree::GraphLogger<Tree> graph(rec, "mission");

  std::int64_t frame = 0;
  for (std::size_t run = 0; run < mission::goals.size(); ++run) {
    beet::StatusTable<Tree> table;
    beet::Runner runner{tree, mission::goals[run], table};

    beet::Status status;
    do {
      status = runner.tick();
      rec.set_time_sequence("tick", frame++);
      const std::string running = graph.log(table);
      rec.log("log", rerun::TextLog("goal " + std::to_string(run) +
                                    ": running [" + running + "]"));
    } while (status == beet::Status::Running);

    rec.log("log", rerun::TextLog("goal " + std::to_string(run) + ": " +
                                  runner.result()->text));
  }
}
