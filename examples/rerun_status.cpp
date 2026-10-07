// Logs the robot mission's node statuses to Rerun as a graph, one frame per tick.
//
//   pixi run -e rerun rerun-example                 # spawns the viewer
//   ./rerun_status --save mission.rrd               # writes a recording instead

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rerun.hpp>

#include "beet/observe.hpp"
#include "robot_mission.hpp"

namespace {

rerun::Color color_of(beet::NodeStatus s) {
  switch (s) {
    case beet::NodeStatus::Idle: return {110, 110, 110};
    case beet::NodeStatus::Running: return {240, 190, 40};
    case beet::NodeStatus::Success: return {60, 180, 80};
    case beet::NodeStatus::Failure: return {220, 60, 60};
    case beet::NodeStatus::Halted: return {150, 90, 200};
  }
  return {255, 255, 255};
}

}  // namespace

int main(int argc, char** argv) {
  const auto tree = mission::make_mission();
  using Tree = std::remove_cvref_t<decltype(tree)>;
  constexpr auto& nodes = beet::tree_info<Tree>;

  const rerun::RecordingStream rec("beet_robot_mission");
  if (argc == 3 && std::string_view(argv[1]) == "--save") {
    rec.save(argv[2]).exit_on_failure();
  } else {
    rec.spawn().exit_on_failure();
  }

  // Static structure, derived entirely from the tree's type.
  std::vector<rerun::components::GraphNode> ids;
  std::vector<rerun::components::Text> labels;
  std::vector<rerun::components::GraphEdge> edges;
  for (std::uint32_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[i];
    ids.emplace_back("n" + std::to_string(i));
    const std::string_view name = n.label.empty() ? n.kind : n.label;
    labels.emplace_back(std::string(name) + "\n" + std::string(n.input) + " -> " + std::string(n.output));
    if (n.parent != beet::no_parent) edges.emplace_back("n" + std::to_string(n.parent), "n" + std::to_string(i));
  }

  // Tidy tree layout: leaves left to right in depth-first order, parents centered over their children.
  // Children always have higher IDs than their parent, so one backwards pass places every parent.
  std::vector<float> x(nodes.size(), 0.0f);
  std::vector<int> depth(nodes.size(), 0);
  std::vector<int> child_count(nodes.size(), 0);
  for (std::uint32_t i = 1; i < nodes.size(); ++i) {
    depth[i] = depth[nodes[i].parent] + 1;
    ++child_count[nodes[i].parent];
  }
  float next_leaf = 0;
  for (std::uint32_t i = 0; i < nodes.size(); ++i) {
    if (child_count[i] == 0) x[i] = 220.0f * next_leaf++;
  }
  std::vector<float> child_sum(nodes.size(), 0.0f);
  for (std::uint32_t i = static_cast<std::uint32_t>(nodes.size()); i-- > 0;) {
    if (child_count[i] > 0) x[i] = child_sum[i] / static_cast<float>(child_count[i]);
    if (nodes[i].parent != beet::no_parent) child_sum[nodes[i].parent] += x[i];
  }
  std::vector<rerun::components::Position2D> positions;
  for (std::uint32_t i = 0; i < nodes.size(); ++i) positions.emplace_back(x[i], 140.0f * static_cast<float>(depth[i]));

  rec.log_static("mission", rerun::GraphEdges(edges).with_graph_type(rerun::components::GraphType::Directed));

  std::int64_t frame = 0;
  for (std::size_t run = 0; run < mission::goals.size(); ++run) {
    beet::StatusTable<Tree> table;
    beet::Runner runner{tree, mission::goals[run], table};

    beet::Status status;
    do {
      status = runner.tick();
      rec.set_time_sequence("tick", frame++);

      std::vector<rerun::components::Color> colors;
      std::string running;
      for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        colors.push_back(color_of(table.status(i)));
        if (table.status(i) == beet::NodeStatus::Running && !nodes[i].label.empty()) {
          running += (running.empty() ? "" : ", ") + std::string(nodes[i].label);
        }
      }
      rec.log("mission", rerun::GraphNodes(ids)
                             .with_labels(labels)
                             .with_colors(colors)
                             .with_positions(positions)
                             .with_show_labels(true));
      rec.log("log", rerun::TextLog("goal " + std::to_string(run) + ": running [" + running + "]"));
    } while (status == beet::Status::Running);

    rec.log("log", rerun::TextLog("goal " + std::to_string(run) + ": " + runner.result()->text));
  }
}
