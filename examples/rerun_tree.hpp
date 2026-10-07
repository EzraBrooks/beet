#pragma once

// Logs a tree's node statuses to Rerun as a graph whose structure comes entirely from the tree's type.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rerun.hpp>

#include "beet/observe.hpp"

namespace rerun_tree {

inline rerun::Color color_of(beet::NodeStatus s) {
  switch (s) {
    case beet::NodeStatus::Idle: return {110, 110, 110};
    case beet::NodeStatus::Running: return {240, 190, 40};
    case beet::NodeStatus::Success: return {60, 180, 80};
    case beet::NodeStatus::Failure: return {220, 60, 60};
    case beet::NodeStatus::Halted: return {150, 90, 200};
  }
  return {255, 255, 255};
}

template <class Tree>
class GraphLogger {
 public:
  GraphLogger(const rerun::RecordingStream& rec, std::string entity) : rec_(rec), entity_(std::move(entity)) {
    std::vector<rerun::components::GraphEdge> edges;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
      const auto& n = nodes[i];
      ids_.emplace_back("n" + std::to_string(i));
      const std::string_view name = n.label.empty() ? n.kind : n.label;
      labels_.emplace_back(std::string(name) + "\n" + std::string(n.input) + " -> " + std::string(n.output));
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
    for (std::uint32_t i = 0; i < nodes.size(); ++i) positions_.emplace_back(x[i], 140.0f * static_cast<float>(depth[i]));

    rec_.log_static(entity_, rerun::GraphEdges(edges).with_graph_type(rerun::components::GraphType::Directed));
  }

  /// Logs every node's current status and returns the labels of the running named nodes, comma separated.
  std::string log(const beet::StatusTable<Tree>& table) const {
    std::vector<rerun::components::Color> colors;
    std::string running;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
      colors.push_back(color_of(table.status(i)));
      if (table.status(i) == beet::NodeStatus::Running && !nodes[i].label.empty()) {
        running += (running.empty() ? "" : ", ") + std::string(nodes[i].label);
      }
    }
    rec_.log(entity_, rerun::GraphNodes(ids_)
                          .with_labels(labels_)
                          .with_colors(colors)
                          .with_positions(positions_)
                          .with_show_labels(true));
    return running;
  }

 private:
  static constexpr auto& nodes = beet::tree_info<Tree>;

  const rerun::RecordingStream& rec_;
  std::string entity_;
  std::vector<rerun::components::GraphNode> ids_;
  std::vector<rerun::components::Text> labels_;
  std::vector<rerun::components::Position2D> positions_;
};

}  // namespace rerun_tree
