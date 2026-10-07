#pragma once

// Logs a tree's node statuses to Rerun as a graph whose structure comes
// entirely from the tree's type.

#include <algorithm>
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
  GraphLogger(const rerun::RecordingStream& rec, std::string entity)
      : rec_(rec), entity_(std::move(entity)) {
    std::vector<rerun::components::GraphEdge> edges;
    std::vector<float> own(nodes.size(), 0.0f);
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
      const auto& n = nodes[i];
      ids_.emplace_back("n" + std::to_string(i));
      const std::string_view name = n.label.empty() ? n.kind : n.label;
      const std::string input = shorten(n.input);
      const std::string output = shorten(n.output);
      labels_.emplace_back(std::string(name) + "\nin: " + input +
                           "\nout: " + output);
      own[i] =
          char_width * static_cast<float>(std::max(
                           {name.size(), input.size() + 4, output.size() + 5}));
      if (n.parent != beet::no_parent)
        edges.emplace_back("n" + std::to_string(n.parent),
                           "n" + std::to_string(i));
    }

    // Tidy tree layout that cannot overlap: every subtree gets its own
    // horizontal span, at least as wide as its root's label and as its
    // children's spans side by side. Children always have higher IDs than their
    // parent, so spans are sized in one backwards pass and placed in one
    // forwards pass.
    std::vector<std::vector<std::uint32_t>> children(nodes.size());
    std::vector<int> depth(nodes.size(), 0);
    for (std::uint32_t i = 1; i < nodes.size(); ++i) {
      children[nodes[i].parent].push_back(i);
      depth[i] = depth[nodes[i].parent] + 1;
    }
    std::vector<float> span(nodes.size(), 0.0f);
    std::vector<float> children_span(nodes.size(), 0.0f);
    for (std::uint32_t i = static_cast<std::uint32_t>(nodes.size()); i-- > 0;) {
      for (std::size_t c = 0; c < children[i].size(); ++c)
        children_span[i] += span[children[i][c]] + (c ? gap : 0.0f);
      span[i] = std::max(own[i], children_span[i]);
    }
    std::vector<float> left(nodes.size(), 0.0f);
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
      float next = left[i] + (span[i] - children_span[i]) / 2;
      for (std::uint32_t c : children[i]) {
        left[c] = next;
        next += span[c] + gap;
      }
      positions_.emplace_back(left[i] + span[i] / 2,
                              row_height * static_cast<float>(depth[i]));
    }

    rec_.log_static(entity_, rerun::GraphEdges(edges).with_graph_type(
                                 rerun::components::GraphType::Directed));
  }

  /// Logs every node's current status and returns the labels of the running
  /// named nodes, comma separated.
  std::string log(const beet::StatusTable<Tree>& table) const {
    std::vector<rerun::components::Color> colors;
    std::string running;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
      colors.push_back(color_of(table.status(i)));
      if (table.status(i) == beet::NodeStatus::Running &&
          !nodes[i].label.empty()) {
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

  // Approximate label metrics in graph units, generous enough for the viewer's
  // proportional font.
  static constexpr float char_width = 7.5f;
  static constexpr float gap = 40.0f;
  static constexpr float row_height = 120.0f;

  static std::string shorten(std::string_view type) {
    constexpr std::string_view anon = "(anonymous namespace)::";
    std::string out(type);
    for (auto at = out.find(anon); at != std::string::npos;
         at = out.find(anon, at))
      out.erase(at, anon.size());
    return out;
  }

  const rerun::RecordingStream& rec_;
  std::string entity_;
  std::vector<rerun::components::GraphNode> ids_;
  std::vector<rerun::components::Text> labels_;
  std::vector<rerun::components::Position2D> positions_;
};

}  // namespace rerun_tree
