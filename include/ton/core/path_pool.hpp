#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <unordered_map>
#include <vector>

#include "ton/core/graph.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"

namespace ton {

inline constexpr double DEFAULT_KAPPA = 0.10;
inline constexpr uint16_t DEFAULT_K_MAX = 10;

struct RawPath {
  std::vector<NodeId> nodes;
  std::vector<ArcId> arcs;
  double cost = 0.0;
};

// Standalone Yen's (kappa, K_max)-bounded simple path generator with strict
// deterministic lexicographic node-ID tie-breaking on equal-cost routes.
[[nodiscard]] std::vector<RawPath> enumerate_yen_paths(
    const Graph& graph, NodeId src, NodeId dst, const CutArcs& cut_arcs = {},
    DistanceMetric metric = DistanceMetric::GeographicKm,
    double kappa = DEFAULT_KAPPA, uint16_t k_max = DEFAULT_K_MAX);

// Lightweight contiguous range of PathIds [first, first + count) inside
// PathPool.
struct PathSlice {
  struct Iterator {
    using iterator_category = std::random_access_iterator_tag;
    using value_type = PathId;
    using difference_type = std::ptrdiff_t;

    PathId current = INVALID_PATH;

    constexpr PathId operator*() const noexcept { return current; }
    constexpr Iterator& operator++() noexcept {
      ++current;
      return *this;
    }
    constexpr Iterator operator++(int) noexcept {
      Iterator tmp = *this;
      ++current;
      return tmp;
    }
    constexpr bool operator==(const Iterator&) const noexcept = default;
  };

  PathId first = INVALID_PATH;
  uint16_t count = 0;

  [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }
  [[nodiscard]] constexpr uint16_t size() const noexcept { return count; }
  [[nodiscard]] constexpr PathId operator[](size_t idx) const noexcept {
    TON_BOUNDS_CHECK(idx < count, "PathSlice::operator[] index out of bounds");
    return static_cast<PathId>(first + idx);
  }
  [[nodiscard]] constexpr Iterator begin() const noexcept {
    return Iterator{first};
  }
  [[nodiscard]] constexpr Iterator end() const noexcept {
    return Iterator{first == INVALID_PATH ? INVALID_PATH
                                          : static_cast<PathId>(first + count)};
  }
};

struct PathMeta {
  uint32_t arc_offset = 0;
  uint32_t node_offset = 0;
  double cost = 0.0;
  uint16_t num_arcs = 0;
  NodeId src = INVALID_NODE;
  NodeId dst = INVALID_NODE;
};

struct DegradedPathKey {
  CutArcs cut;
  NodeId src = INVALID_NODE;
  NodeId dst = INVALID_NODE;

  [[nodiscard]] constexpr bool operator==(
      const DegradedPathKey&) const noexcept = default;
};

struct DegradedPathKeyHash {
  [[nodiscard]] size_t operator()(const DegradedPathKey& key) const noexcept {
    uint64_t h = (static_cast<uint64_t>(key.src) << 48) ^
                 (static_cast<uint64_t>(key.dst) << 32) ^
                 static_cast<uint64_t>(key.cut.count);
    for (uint8_t i = 0; i < key.cut.count; ++i) {
      h ^= static_cast<uint64_t>(key.cut.arcs[i]) + 0x9e3779b97f4a7c15ULL +
           (h << 6) + (h >> 2);
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return static_cast<size_t>(h);
  }
};

class PathPool {
 public:
  explicit PathPool(const Graph& graph,
                    DistanceMetric metric = DistanceMetric::GeographicKm,
                    double kappa = DEFAULT_KAPPA,
                    uint16_t k_max = DEFAULT_K_MAX);

  [[nodiscard]] const Graph& graph() const noexcept { return *graph_; }
  [[nodiscard]] DistanceMetric metric() const noexcept { return metric_; }
  [[nodiscard]] double kappa() const noexcept { return kappa_; }
  [[nodiscard]] uint16_t k_max() const noexcept { return k_max_; }
  [[nodiscard]] size_t num_paths() const noexcept { return paths_.size(); }

  // Mutating memoized queries (for setup / prewarming).
  [[nodiscard]] PathSlice get_or_compute_healthy(NodeId src, NodeId dst);
  [[nodiscard]] PathSlice get_or_compute(const CutArcs& cut_arcs, NodeId src,
                                         NodeId dst);

  // Precomputes healthy paths for all N*(N-1) ordered pairs.
  void prewarm_all_healthy();

  // Precomputes all healthy, Stage-1, and dual-cut candidate route sets
  // reachable by `requests`, freezing the flat arena so online provisioning
  // performs zero dynamic memory allocations.
  void prewarm_for_stream(std::span<const Request> requests,
                          const ScenarioSet& scenarios);

  // Zero-allocation read-only lookups for the online provisioning loop (t =
  // 0..999).
  [[nodiscard]] PathSlice find_healthy(NodeId src, NodeId dst) const noexcept;
  [[nodiscard]] PathSlice find(const CutArcs& cut_arcs, NodeId src,
                               NodeId dst) const noexcept;

  [[nodiscard]] const PathMeta& path_meta(PathId pid) const noexcept {
    TON_BOUNDS_CHECK(pid < paths_.size(),
                     "PathPool::path_meta PathId out of bounds");
    return paths_[pid];
  }
  [[nodiscard]] double path_cost(PathId pid) const noexcept {
    TON_BOUNDS_CHECK(pid < paths_.size(),
                     "PathPool::path_cost PathId out of bounds");
    return paths_[pid].cost;
  }
  [[nodiscard]] uint16_t path_hops(PathId pid) const noexcept {
    TON_BOUNDS_CHECK(pid < paths_.size(),
                     "PathPool::path_hops PathId out of bounds");
    return paths_[pid].num_arcs;
  }

  [[nodiscard]] std::span<const ArcId> path_arcs(PathId pid) const noexcept {
    TON_BOUNDS_CHECK(pid < paths_.size(),
                     "PathPool::path_arcs PathId out of bounds");
    const auto& m = paths_[pid];
    return std::span<const ArcId>(arc_pool_.data() + m.arc_offset, m.num_arcs);
  }

  [[nodiscard]] std::span<const NodeId> path_nodes(PathId pid) const noexcept {
    TON_BOUNDS_CHECK(pid < paths_.size(),
                     "PathPool::path_nodes PathId out of bounds");
    const auto& m = paths_[pid];
    return std::span<const NodeId>(node_pool_.data() + m.node_offset,
                                   static_cast<size_t>(m.num_arcs) + 1);
  }

  [[nodiscard]] bool path_intersects(PathId pid,
                                     const CutArcs& cut) const noexcept {
    for (const ArcId a : path_arcs(pid)) {
      if (cut.contains(a)) {
        return true;
      }
    }
    return false;
  }

 private:
  PathSlice append_paths(NodeId src, NodeId dst,
                         const std::vector<RawPath>& raw_paths);

  const Graph* graph_ = nullptr;
  DistanceMetric metric_ = DistanceMetric::GeographicKm;
  double kappa_ = DEFAULT_KAPPA;
  uint16_t k_max_ = DEFAULT_K_MAX;

  std::vector<PathMeta> paths_;
  std::vector<ArcId> arc_pool_;
  std::vector<NodeId> node_pool_;

  // Healthy G table of size N * N; sentinel first == INVALID_PATH && count ==
  // 0xFFFFu means uncached.
  static constexpr uint16_t kUncachedCount = 0xFFFFu;
  std::vector<PathSlice> healthy_table_;
  std::unordered_map<DegradedPathKey, PathSlice, DegradedPathKeyHash>
      degraded_table_;
};

}  // namespace ton
