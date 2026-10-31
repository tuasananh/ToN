#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ton/core/types.hpp"

namespace ton {

struct NodeInfo {
  NodeId id = INVALID_NODE;
  std::string name;
  double lon_deg = 0.0;
  double lat_deg = 0.0;
};

struct SpanInput {
  std::string name;
  NodeId u = INVALID_NODE;
  NodeId v = INVALID_NODE;
  // If > 0.0, overrides Haversine coordinate distance (useful for exact
  // synthetic unit tests).
  double override_length_km = 0.0;
};

struct Span {
  SpanId id = INVALID_SPAN;
  std::string name;
  NodeId u = INVALID_NODE;
  NodeId v = INVALID_NODE;
  ArcId arc_uv = INVALID_ARC;
  ArcId arc_vu = INVALID_ARC;
  double length_km = 0.0;
};

struct Arc {
  ArcId id = INVALID_ARC;
  NodeId src = INVALID_NODE;
  NodeId dst = INVALID_NODE;
  SpanId span_id = INVALID_SPAN;
  ArcId reverse_arc = INVALID_ARC;
  double length_km = 0.0;
};

class Graph {
 public:
  Graph() = default;

  Graph(std::string name, std::vector<NodeInfo> nodes,
        const std::vector<SpanInput>& spans,
        DistanceMetric default_metric = DistanceMetric::GeographicKm);

  [[nodiscard]] static Graph from_sndlib_xml(
      const std::filesystem::path& xml_path,
      DistanceMetric default_metric = DistanceMetric::GeographicKm);

  [[nodiscard]] static double haversine_km(double lon1_deg, double lat1_deg,
                                           double lon2_deg,
                                           double lat2_deg) noexcept;

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] DistanceMetric default_metric() const noexcept {
    return default_metric_;
  }

  [[nodiscard]] NodeId num_nodes() const noexcept {
    return static_cast<NodeId>(nodes_.size());
  }
  [[nodiscard]] SpanId num_spans() const noexcept {
    return static_cast<SpanId>(spans_.size());
  }
  [[nodiscard]] ArcId num_arcs() const noexcept {
    return static_cast<ArcId>(arcs_.size());
  }

  [[nodiscard]] const NodeInfo& node(NodeId id) const noexcept {
    TON_BOUNDS_CHECK(id < nodes_.size(), "Graph::node NodeId out of bounds");
    return nodes_[id];
  }
  [[nodiscard]] const Span& span(SpanId id) const noexcept {
    TON_BOUNDS_CHECK(id < spans_.size(), "Graph::span SpanId out of bounds");
    return spans_[id];
  }
  [[nodiscard]] const Arc& arc(ArcId id) const noexcept {
    TON_BOUNDS_CHECK(id < arcs_.size(), "Graph::arc ArcId out of bounds");
    return arcs_[id];
  }

  [[nodiscard]] std::span<const NodeInfo> nodes() const noexcept {
    return nodes_;
  }
  [[nodiscard]] std::span<const Span> spans() const noexcept { return spans_; }
  [[nodiscard]] std::span<const Arc> arcs() const noexcept { return arcs_; }

  // Outgoing arcs from node u, deterministically sorted by ascending target
  // NodeId.
  [[nodiscard]] std::span<const ArcId> out_arcs(NodeId u) const noexcept {
    TON_BOUNDS_CHECK(u < nodes_.size(), "Graph::out_arcs NodeId out of bounds");
    const uint32_t begin = adj_offsets_[u];
    const uint32_t end = adj_offsets_[static_cast<size_t>(u) + 1];
    return std::span<const ArcId>(adj_arcs_.data() + begin, end - begin);
  }

  [[nodiscard]] double arc_cost(ArcId a, DistanceMetric metric) const noexcept {
    TON_BOUNDS_CHECK(a < arcs_.size(), "Graph::arc_cost ArcId out of bounds");
    return metric == DistanceMetric::GeographicKm ? arcs_[a].length_km : 1.0;
  }

  [[nodiscard]] double arc_cost(ArcId a) const noexcept {
    return arc_cost(a, default_metric_);
  }

  [[nodiscard]] ArcId find_arc(NodeId src, NodeId dst) const noexcept;
  [[nodiscard]] NodeId find_node(std::string_view node_name) const noexcept;

 private:
  std::string name_;
  DistanceMetric default_metric_ = DistanceMetric::GeographicKm;
  std::vector<NodeInfo> nodes_;
  std::vector<Span> spans_;
  std::vector<Arc> arcs_;
  std::vector<uint32_t> adj_offsets_;
  std::vector<ArcId> adj_arcs_;
  std::unordered_map<std::string, NodeId> name_to_id_;
};

}  // namespace ton
