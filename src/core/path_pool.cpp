#include "ton/core/path_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ton {

namespace {

constexpr double kStretchEps = 1e-9;
constexpr double kCostKeyScale = 1e9;  // 1 micrometer resolution in km; exact integer for hops.
constexpr size_t kMaxStaticNodes = 128;

[[nodiscard]] int64_t quantize_cost_key(double cost) noexcept {
    return static_cast<int64_t>(std::llround(cost * kCostKeyScale));
}

struct CandidateRoute {
    std::vector<NodeId> nodes;
    std::vector<ArcId> arcs;
    double cost = 0.0;
    int64_t cost_key = 0;
};

[[nodiscard]] bool candidate_less(const CandidateRoute& a, const CandidateRoute& b) noexcept {
    if (a.cost_key != b.cost_key) {
        return a.cost_key < b.cost_key;
    }
    return std::lexicographical_compare(a.nodes.begin(), a.nodes.end(), b.nodes.begin(),
                                        b.nodes.end());
}

// Stack-friendly fixed node sequence for O(N^2) lexicographic Forward Dijkstra when N <= 128.
struct StaticNodeSeq {
    std::array<NodeId, kMaxStaticNodes> data{};
    uint8_t len = 0;
    std::array<ArcId, kMaxStaticNodes> arcs{};
};

[[nodiscard]] bool lex_less_append(const StaticNodeSeq& prefix, NodeId next_node,
                                   const StaticNodeSeq& rhs) noexcept {
    const size_t lhs_len = static_cast<size_t>(prefix.len) + 1;
    const size_t rhs_len = rhs.len;
    const size_t min_len = std::min(lhs_len, rhs_len);
    for (size_t i = 0; i < min_len; ++i) {
        const NodeId lv = (i < prefix.len) ? prefix.data[i] : next_node;
        const NodeId rv = rhs.data[i];
        if (lv != rv) {
            return lv < rv;
        }
    }
    return lhs_len < rhs_len;
}

[[nodiscard]] bool lex_less_seq(const StaticNodeSeq& a, const StaticNodeSeq& b) noexcept {
    const size_t min_len = std::min(a.len, b.len);
    for (size_t i = 0; i < min_len; ++i) {
        if (a.data[i] != b.data[i]) {
            return a.data[i] < b.data[i];
        }
    }
    return a.len < b.len;
}

// Exact lexicographic Forward Dijkstra from `spur` to `dst`, avoiding:
//   - arcs in `cut_arcs`,
//   - arcs in `forbidden_spur_arcs` (all outgoing from `spur`),
//   - vertices with `forbidden_node[v] != 0`.
bool lexicographic_spur_dijkstra(const Graph& graph, NodeId spur, NodeId dst,
                                 const CutArcs& cut_arcs, std::span<const uint8_t> forbidden_nodes,
                                 std::span<const ArcId> forbidden_spur_arcs, DistanceMetric metric,
                                 std::vector<NodeId>& out_nodes, std::vector<ArcId>& out_arcs) {
    const size_t n = graph.num_nodes();
    const auto is_spur_arc_forbidden = [&](ArcId a) noexcept {
        for (const ArcId fa : forbidden_spur_arcs) {
            if (fa == a) {
                return true;
            }
        }
        return false;
    };

    if (n <= kMaxStaticNodes) {
        constexpr int64_t kInfKey = std::numeric_limits<int64_t>::max();
        std::array<double, kMaxStaticNodes> dist_raw;
        std::array<int64_t, kMaxStaticNodes> dist_key;
        std::array<uint8_t, kMaxStaticNodes> settled;
        std::array<StaticNodeSeq, kMaxStaticNodes> best_seq;

        std::fill_n(dist_raw.begin(), n, std::numeric_limits<double>::infinity());
        std::fill_n(dist_key.begin(), n, kInfKey);
        std::fill_n(settled.begin(), n, uint8_t{0});

        dist_raw[spur] = 0.0;
        dist_key[spur] = 0;
        best_seq[spur].data[0] = spur;
        best_seq[spur].len = 1;

        for (size_t iter = 0; iter < n; ++iter) {
            NodeId u = INVALID_NODE;
            for (NodeId cand = 0; cand < n; ++cand) {
                if (settled[cand] != 0 || dist_key[cand] == kInfKey) {
                    continue;
                }
                if (u == INVALID_NODE || dist_key[cand] < dist_key[u] ||
                    (dist_key[cand] == dist_key[u] && lex_less_seq(best_seq[cand], best_seq[u]))) {
                    u = cand;
                }
            }
            if (u == INVALID_NODE) {
                break;
            }
            settled[u] = 1;
            if (u == dst) {
                out_nodes.assign(best_seq[dst].data.begin(),
                                 best_seq[dst].data.begin() + best_seq[dst].len);
                out_arcs.assign(best_seq[dst].arcs.begin(),
                                best_seq[dst].arcs.begin() + (best_seq[dst].len - 1));
                return true;
            }

            const auto out = graph.out_arcs(u);
            for (const ArcId a : out) {
                if (cut_arcs.contains(a)) {
                    continue;
                }
                if (u == spur && is_spur_arc_forbidden(a)) {
                    continue;
                }
                const NodeId v = graph.arc(a).dst;
                if (settled[v] != 0 || forbidden_nodes[v] != 0) {
                    continue;
                }
                const double nd_raw = dist_raw[u] + graph.arc_cost(a, metric);
                const int64_t nd_key = quantize_cost_key(nd_raw);
                if (nd_key < dist_key[v] ||
                    (nd_key == dist_key[v] && lex_less_append(best_seq[u], v, best_seq[v]))) {
                    dist_raw[v] = nd_raw;
                    dist_key[v] = nd_key;
                    best_seq[v] = best_seq[u];
                    best_seq[v].arcs[best_seq[v].len - 1] = a;
                    best_seq[v].data[best_seq[v].len] = v;
                    ++best_seq[v].len;
                }
            }
        }
        return false;
    }

    // General fallback for graphs with N > 128.
    constexpr int64_t kInfKey = std::numeric_limits<int64_t>::max();
    std::vector<double> dist_raw(n, std::numeric_limits<double>::infinity());
    std::vector<int64_t> dist_key(n, kInfKey);
    std::vector<uint8_t> settled(n, 0);
    std::vector<std::vector<NodeId>> best_nodes(n);
    std::vector<std::vector<ArcId>> best_arcs(n);

    dist_raw[spur] = 0.0;
    dist_key[spur] = 0;
    best_nodes[spur] = {spur};

    for (size_t iter = 0; iter < n; ++iter) {
        NodeId u = INVALID_NODE;
        for (NodeId cand = 0; cand < n; ++cand) {
            if (settled[cand] != 0 || dist_key[cand] == kInfKey) {
                continue;
            }
            if (u == INVALID_NODE || dist_key[cand] < dist_key[u] ||
                (dist_key[cand] == dist_key[u] &&
                 std::lexicographical_compare(best_nodes[cand].begin(), best_nodes[cand].end(),
                                              best_nodes[u].begin(), best_nodes[u].end()))) {
                u = cand;
            }
        }
        if (u == INVALID_NODE) {
            break;
        }
        settled[u] = 1;
        if (u == dst) {
            out_nodes = std::move(best_nodes[dst]);
            out_arcs = std::move(best_arcs[dst]);
            return true;
        }

        for (const ArcId a : graph.out_arcs(u)) {
            if (cut_arcs.contains(a) || (u == spur && is_spur_arc_forbidden(a))) {
                continue;
            }
            const NodeId v = graph.arc(a).dst;
            if (settled[v] != 0 || forbidden_nodes[v] != 0) {
                continue;
            }
            const double nd_raw = dist_raw[u] + graph.arc_cost(a, metric);
            const int64_t nd_key = quantize_cost_key(nd_raw);
            std::vector<NodeId> cand_nodes = best_nodes[u];
            cand_nodes.push_back(v);
            if (nd_key < dist_key[v] ||
                (nd_key == dist_key[v] &&
                 std::lexicographical_compare(cand_nodes.begin(), cand_nodes.end(),
                                              best_nodes[v].begin(), best_nodes[v].end()))) {
                dist_raw[v] = nd_raw;
                dist_key[v] = nd_key;
                best_nodes[v] = std::move(cand_nodes);
                best_arcs[v] = best_arcs[u];
                best_arcs[v].push_back(a);
            }
        }
    }
    return false;
}

[[nodiscard]] double compute_canonical_path_cost(const Graph& graph, std::span<const ArcId> arcs,
                                                 DistanceMetric metric) noexcept {
    double total = 0.0;
    for (const ArcId a : arcs) {
        total += graph.arc_cost(a, metric);
    }
    return total;
}

}  // namespace

std::vector<RawPath> enumerate_yen_paths(const Graph& graph, NodeId src, NodeId dst,
                                         const CutArcs& cut_arcs, DistanceMetric metric,
                                         double kappa, uint16_t k_max) {
    if (src >= graph.num_nodes() || dst >= graph.num_nodes() || src == dst || k_max == 0 ||
        kappa < 0.0) {
        return {};
    }

    const size_t n = graph.num_nodes();
    std::vector<uint8_t> forbidden_nodes(n, 0);
    std::vector<ArcId> forbidden_spur_arcs;
    forbidden_spur_arcs.reserve(k_max);

    std::vector<NodeId> first_nodes;
    std::vector<ArcId> first_arcs;
    if (!lexicographic_spur_dijkstra(graph, src, dst, cut_arcs, forbidden_nodes, {}, metric,
                                     first_nodes, first_arcs)) {
        return {};
    }

    const double shortest_cost = compute_canonical_path_cost(graph, first_arcs, metric);
    const double max_allowed_cost = (1.0 + kappa) * shortest_cost + kStretchEps;

    std::vector<CandidateRoute> accepted;
    accepted.reserve(k_max);
    accepted.push_back(CandidateRoute{
        .nodes = std::move(first_nodes),
        .arcs = std::move(first_arcs),
        .cost = shortest_cost,
        .cost_key = quantize_cost_key(shortest_cost),
    });

    std::vector<CandidateRoute> pool_b;
    std::vector<NodeId> spur_nodes;
    std::vector<ArcId> spur_arcs;

    for (uint16_t k = 1; k < k_max; ++k) {
        const CandidateRoute& prev = accepted.back();
        const size_t num_hops = prev.arcs.size();

        double root_cost = 0.0;
        for (size_t i = 0; i < num_hops; ++i) {
            if (i > 0) {
                root_cost += graph.arc_cost(prev.arcs[i - 1], metric);
            }
            if (root_cost > max_allowed_cost) {
                break;
            }

            const NodeId spur_node = prev.nodes[i];
            std::fill(forbidden_nodes.begin(), forbidden_nodes.end(), uint8_t{0});
            for (size_t r = 0; r < i; ++r) {
                forbidden_nodes[prev.nodes[r]] = 1;
            }

            forbidden_spur_arcs.clear();
            for (const auto& p : accepted) {
                if (p.nodes.size() > i &&
                    std::equal(prev.nodes.begin(),
                               prev.nodes.begin() + static_cast<std::ptrdiff_t>(i + 1),
                               p.nodes.begin())) {
                    forbidden_spur_arcs.push_back(p.arcs[i]);
                }
            }

            if (!lexicographic_spur_dijkstra(graph, spur_node, dst, cut_arcs, forbidden_nodes,
                                             forbidden_spur_arcs, metric, spur_nodes, spur_arcs)) {
                continue;
            }

            std::vector<NodeId> total_nodes;
            total_nodes.reserve(i + spur_nodes.size());
            total_nodes.insert(total_nodes.end(), prev.nodes.begin(),
                               prev.nodes.begin() + static_cast<std::ptrdiff_t>(i));
            total_nodes.insert(total_nodes.end(), spur_nodes.begin(), spur_nodes.end());

            std::vector<ArcId> total_arcs;
            total_arcs.reserve(i + spur_arcs.size());
            total_arcs.insert(total_arcs.end(), prev.arcs.begin(),
                              prev.arcs.begin() + static_cast<std::ptrdiff_t>(i));
            total_arcs.insert(total_arcs.end(), spur_arcs.begin(), spur_arcs.end());

            const double total_cost = compute_canonical_path_cost(graph, total_arcs, metric);
            if (total_cost > max_allowed_cost) {
                continue;
            }

            bool duplicate = false;
            for (const auto& existing : accepted) {
                if (existing.nodes == total_nodes) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                for (const auto& existing : pool_b) {
                    if (existing.nodes == total_nodes) {
                        duplicate = true;
                        break;
                    }
                }
            }
            if (!duplicate) {
                pool_b.push_back(CandidateRoute{
                    .nodes = std::move(total_nodes),
                    .arcs = std::move(total_arcs),
                    .cost = total_cost,
                    .cost_key = quantize_cost_key(total_cost),
                });
            }
        }

        if (pool_b.empty()) {
            break;
        }

        const auto best_it = std::min_element(pool_b.begin(), pool_b.end(), candidate_less);
        if (best_it->cost > max_allowed_cost) {
            break;
        }
        accepted.push_back(std::move(*best_it));
        pool_b.erase(best_it);
    }

    std::vector<RawPath> result;
    result.reserve(accepted.size());
    for (auto& route : accepted) {
        result.push_back(RawPath{
            .nodes = std::move(route.nodes),
            .arcs = std::move(route.arcs),
            .cost = route.cost,
        });
    }
    return result;
}

PathPool::PathPool(const Graph& graph, DistanceMetric metric, double kappa, uint16_t k_max)
    : graph_(&graph), metric_(metric), kappa_(kappa), k_max_(k_max) {
    if (kappa < 0.0 || k_max == 0) {
        throw std::invalid_argument("PathPool requires kappa >= 0 and k_max >= 1");
    }
    const size_t n = graph.num_nodes();
    healthy_table_.assign(n * n, PathSlice{.first = INVALID_PATH, .count = kUncachedCount});
}

PathSlice PathPool::append_paths(NodeId src, NodeId dst, const std::vector<RawPath>& raw_paths) {
    if (raw_paths.empty()) {
        return PathSlice{.first = INVALID_PATH, .count = 0};
    }
    const auto first_id = static_cast<PathId>(paths_.size());
    for (const auto& rp : raw_paths) {
        const auto a_off = static_cast<uint32_t>(arc_pool_.size());
        const auto n_off = static_cast<uint32_t>(node_pool_.size());
        arc_pool_.insert(arc_pool_.end(), rp.arcs.begin(), rp.arcs.end());
        node_pool_.insert(node_pool_.end(), rp.nodes.begin(), rp.nodes.end());
        paths_.push_back(PathMeta{
            .arc_offset = a_off,
            .node_offset = n_off,
            .cost = rp.cost,
            .num_arcs = static_cast<uint16_t>(rp.arcs.size()),
            .src = src,
            .dst = dst,
        });
    }
    return PathSlice{
        .first = first_id,
        .count = static_cast<uint16_t>(raw_paths.size()),
    };
}

PathSlice PathPool::get_or_compute_healthy(NodeId src, NodeId dst) {
    const size_t n = graph_->num_nodes();
    if (src >= n || dst >= n || src == dst) {
        return PathSlice{.first = INVALID_PATH, .count = 0};
    }
    PathSlice& slot = healthy_table_[static_cast<size_t>(src) * n + dst];
    if (slot.count != kUncachedCount) {
        return slot;
    }
    const auto raw = enumerate_yen_paths(*graph_, src, dst, CutArcs{}, metric_, kappa_, k_max_);
    slot = append_paths(src, dst, raw);
    return slot;
}

PathSlice PathPool::get_or_compute(const CutArcs& cut_arcs, NodeId src, NodeId dst) {
    if (cut_arcs.count == 0) {
        return get_or_compute_healthy(src, dst);
    }
    const size_t n = graph_->num_nodes();
    if (src >= n || dst >= n || src == dst) {
        return PathSlice{.first = INVALID_PATH, .count = 0};
    }
    const DegradedPathKey key{.cut = cut_arcs, .src = src, .dst = dst};
    if (const auto it = degraded_table_.find(key); it != degraded_table_.end()) {
        return it->second;
    }
    const auto raw = enumerate_yen_paths(*graph_, src, dst, cut_arcs, metric_, kappa_, k_max_);
    const PathSlice slice = append_paths(src, dst, raw);
    degraded_table_.emplace(key, slice);
    return slice;
}

PathSlice PathPool::find_healthy(NodeId src, NodeId dst) const noexcept {
    const size_t n = graph_->num_nodes();
    if (src >= n || dst >= n || src == dst) {
        return PathSlice{.first = INVALID_PATH, .count = 0};
    }
    const PathSlice slot = healthy_table_[static_cast<size_t>(src) * n + dst];
    return slot.count == kUncachedCount ? PathSlice{.first = INVALID_PATH, .count = 0} : slot;
}

PathSlice PathPool::find(const CutArcs& cut_arcs, NodeId src, NodeId dst) const noexcept {
    if (cut_arcs.count == 0) {
        return find_healthy(src, dst);
    }
    const DegradedPathKey key{.cut = cut_arcs, .src = src, .dst = dst};
    const auto it = degraded_table_.find(key);
    return it == degraded_table_.end() ? PathSlice{.first = INVALID_PATH, .count = 0} : it->second;
}

void PathPool::prewarm_all_healthy() {
    const NodeId n = graph_->num_nodes();
    for (NodeId u = 0; u < n; ++u) {
        for (NodeId v = 0; v < n; ++v) {
            if (u != v) {
                (void)get_or_compute_healthy(u, v);
            }
        }
    }
}

void PathPool::prewarm_for_stream(std::span<const Request> requests, const ScenarioSet& scenarios) {
    const size_t n = graph_->num_nodes();
    const size_t m = graph_->num_arcs();
    std::vector<uint8_t> pair_seen(n * n, 0);
    std::vector<std::pair<NodeId, NodeId>> unique_pairs;
    unique_pairs.reserve(requests.size());

    for (const auto& req : requests) {
        if (req.src < n && req.dst < n && req.src != req.dst) {
            const size_t idx = static_cast<size_t>(req.src) * n + req.dst;
            if (pair_seen[idx] == 0) {
                pair_seen[idx] = 1;
                unique_pairs.emplace_back(req.src, req.dst);
            }
        }
    }

    std::vector<uint8_t> touched_arcs(m, 0);
    std::vector<uint8_t> visited_sim(scenarios.sim_scenarios().size(), 0);
    std::vector<ScenarioId> dirty_sim;
    std::vector<uint8_t> visited_s1(scenarios.num_units(), 0);
    std::vector<FailureUnitId> dirty_s1;

    for (const auto& [src, dst] : unique_pairs) {
        const PathSlice healthy = get_or_compute_healthy(src, dst);
        std::fill(touched_arcs.begin(), touched_arcs.end(), uint8_t{0});
        for (const PathId pid : healthy) {
            for (const ArcId a : path_arcs(pid)) {
                touched_arcs[a] = 1;
            }
        }

        dirty_sim.clear();
        dirty_s1.clear();
        for (ArcId a = 0; a < m; ++a) {
            if (touched_arcs[a] == 0) {
                continue;
            }
            const FailureUnitId u_id = scenarios.arc_to_unit(a);
            if (u_id != INVALID_FAILURE_UNIT && scenarios.is_active_stage1_unit(u_id) &&
                visited_s1[u_id] == 0) {
                visited_s1[u_id] = 1;
                dirty_s1.push_back(u_id);
            }
            for (const ScenarioId sim_id : scenarios.sim_scenarios_cutting_arc(a)) {
                if (visited_sim[sim_id] == 0) {
                    visited_sim[sim_id] = 1;
                    dirty_sim.push_back(sim_id);
                }
            }
        }

        for (const FailureUnitId x1 : dirty_s1) {
            (void)get_or_compute(scenarios.unit_cut_arcs(x1), src, dst);
            visited_s1[x1] = 0;
        }
        for (const ScenarioId sim_id : dirty_sim) {
            (void)get_or_compute(scenarios.sim_scenario(sim_id).cut_arcs, src, dst);
            visited_sim[sim_id] = 0;
        }
    }
}

}  // namespace ton
