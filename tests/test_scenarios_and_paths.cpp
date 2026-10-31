#include <cmath>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

#include "test_helpers.hpp"

namespace {

// Brute-force all-simple-paths DFS oracle to verify Yen's (kappa, K_max) &
// lexicographic ties!
std::vector<ton::RawPath> brute_force_simple_paths_oracle(
    const ton::Graph& g, ton::NodeId src, ton::NodeId dst,
    const ton::CutArcs& cut_arcs, ton::DistanceMetric metric, double kappa,
    uint16_t k_max) {
  std::vector<ton::RawPath> all_simple;
  std::vector<uint8_t> on_path(g.num_nodes(), 0);
  std::vector<ton::NodeId> cur_nodes = {src};
  std::vector<ton::ArcId> cur_arcs;
  on_path[src] = 1;

  double best_shortest = std::numeric_limits<double>::infinity();
  auto dfs_shortest = [&](auto& self, ton::NodeId u,
                          double cost_so_far) -> void {
    if (cost_so_far >= best_shortest) {
      return;
    }
    if (u == dst) {
      best_shortest = cost_so_far;
      return;
    }
    for (const ton::ArcId a : g.out_arcs(u)) {
      if (cut_arcs.contains(a)) {
        continue;
      }
      const ton::NodeId v = g.arc(a).dst;
      if (on_path[v] != 0) {
        continue;
      }
      on_path[v] = 1;
      self(self, v, cost_so_far + g.arc_cost(a, metric));
      on_path[v] = 0;
    }
  };
  dfs_shortest(dfs_shortest, src, 0.0);
  if (!std::isfinite(best_shortest)) {
    return {};
  }

  const double max_allowed = (1.0 + kappa) * best_shortest + 1e-9;

  auto dfs_collect = [&](auto& self, ton::NodeId u,
                         double cost_so_far) -> void {
    if (cost_so_far > max_allowed) {
      return;
    }
    if (u == dst) {
      double exact_cost = 0.0;
      for (const ton::ArcId a : cur_arcs) {
        exact_cost += g.arc_cost(a, metric);
      }
      if (exact_cost <= max_allowed) {
        all_simple.push_back(ton::RawPath{
            .nodes = cur_nodes,
            .arcs = cur_arcs,
            .cost = exact_cost,
        });
      }
      return;
    }
    for (const ton::ArcId a : g.out_arcs(u)) {
      if (cut_arcs.contains(a)) {
        continue;
      }
      const ton::NodeId v = g.arc(a).dst;
      if (on_path[v] != 0) {
        continue;
      }
      on_path[v] = 1;
      cur_nodes.push_back(v);
      cur_arcs.push_back(a);
      self(self, v, cost_so_far + g.arc_cost(a, metric));
      cur_arcs.pop_back();
      cur_nodes.pop_back();
      on_path[v] = 0;
    }
  };
  dfs_collect(dfs_collect, src, 0.0);

  std::sort(all_simple.begin(), all_simple.end(),
            [](const ton::RawPath& a, const ton::RawPath& b) {
              const auto ka = static_cast<int64_t>(std::llround(a.cost * 1e9));
              const auto kb = static_cast<int64_t>(std::llround(b.cost * 1e9));
              if (ka != kb) {
                return ka < kb;
              }
              return a.nodes < b.nodes;
            });
  if (all_simple.size() > k_max) {
    all_simple.resize(k_max);
  }
  return all_simple;
}

}  // namespace

TEST_CASE("Dual-cut scenario enumerator and Tarjan SCC assertions",
          "[core][scenarios]") {
  SECTION("Synthetic 4-cycle square connectivity") {
    std::vector<ton::NodeInfo> nodes = {
        {.id = 0, .name = "N0", .lon_deg = 0.0, .lat_deg = 0.0},
        {.id = 1, .name = "N1", .lon_deg = 1.0, .lat_deg = 0.0},
        {.id = 2, .name = "N2", .lon_deg = 1.0, .lat_deg = 1.0},
        {.id = 3, .name = "N3", .lon_deg = 0.0, .lat_deg = 1.0},
    };
    std::vector<ton::SpanInput> spans = {
        {.name = "S01", .u = 0, .v = 1, .override_length_km = 10.0},
        {.name = "S12", .u = 1, .v = 2, .override_length_km = 10.0},
        {.name = "S23", .u = 2, .v = 3, .override_length_km = 10.0},
        {.name = "S30", .u = 3, .v = 0, .override_length_km = 10.0},
    };
    const ton::Graph square("square4", nodes, spans);
    REQUIRE(ton::is_strongly_connected(square, ton::CutArcs{}));

    const auto square_span =
        ton::ScenarioSet::enumerate(square, ton::CutModel::Span);
    REQUIRE(square_span.sim_scenarios().empty());
    REQUIRE(square_span.seq_scenarios().empty());

    const auto square_dir =
        ton::ScenarioSet::enumerate(square, ton::CutModel::Directional);
    REQUIRE(square_dir.sim_scenarios().size() == 16);
    REQUIRE(square_dir.seq_scenarios().size() == 32);
  }

  SECTION("Exact scenario counts on Nobel-EU and Germany50") {
    const auto nobel = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
    const auto germany = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "germany50.xml");

    const auto nobel_span =
        ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
    REQUIRE(nobel_span.sim_scenarios().size() == 804);
    REQUIRE(nobel_span.seq_scenarios().size() == 1608);

    const auto nobel_dir =
        ton::ScenarioSet::enumerate(nobel, ton::CutModel::Directional);
    REQUIRE(nobel_dir.sim_scenarios().size() == 3289);
    REQUIRE(nobel_dir.seq_scenarios().size() == 6578);

    const auto germany_span =
        ton::ScenarioSet::enumerate(germany, ton::CutModel::Span);
    REQUIRE(germany_span.sim_scenarios().size() == 3817);
    REQUIRE(germany_span.seq_scenarios().size() == 7634);

    const auto germany_dir =
        ton::ScenarioSet::enumerate(germany, ton::CutModel::Directional);
    REQUIRE(germany_dir.sim_scenarios().size() == 15378);
    REQUIRE(germany_dir.seq_scenarios().size() == 30756);

    for (const auto& [g_ptr, sc_ptr] : {
             std::pair{&nobel, &nobel_span},
             std::pair{&nobel, &nobel_dir},
             std::pair{&germany, &germany_span},
             std::pair{&germany, &germany_dir},
         }) {
      const auto& g = *g_ptr;
      const auto& sc_set = *sc_ptr;
      REQUIRE(sc_set.seq_scenarios().size() ==
              2 * sc_set.sim_scenarios().size());

      size_t total_grouped = 0;
      for (const ton::FailureUnitId x1 : sc_set.active_stage1_units()) {
        REQUIRE(sc_set.is_active_stage1_unit(x1));
        REQUIRE(ton::is_strongly_connected(g, sc_set.unit_cut_arcs(x1)));
        const auto grp = sc_set.seq_group(x1);
        REQUIRE_FALSE(grp.empty());
        total_grouped += grp.size();
        for (const auto& seq_sc : grp) {
          REQUIRE(seq_sc.x1 == x1);
          REQUIRE(seq_sc.x2 != x1);
          const auto& sim_sc = sc_set.sim_scenario(seq_sc.sim_id);
          REQUIRE(sim_sc.cut_arcs == seq_sc.total_cut);
          REQUIRE(((sim_sc.x1 == seq_sc.x1 && sim_sc.x2 == seq_sc.x2) ||
                   (sim_sc.x1 == seq_sc.x2 && sim_sc.x2 == seq_sc.x1)));
        }
      }
      REQUIRE(total_grouped == sc_set.seq_scenarios().size());

      for (ton::ArcId a = 0; a < g.num_arcs(); ++a) {
        for (const ton::ScenarioId sim_id :
             sc_set.sim_scenarios_cutting_arc(a)) {
          const auto& sc = sc_set.sim_scenario(sim_id);
          REQUIRE(sc.cut_arcs.contains(a));
        }
        for (const ton::ScenarioId seq_id :
             sc_set.seq_scenarios_stage2_cutting_arc(a)) {
          const auto& sc = sc_set.seq_scenario(seq_id);
          REQUIRE(sc.stage2_cut.contains(a));
        }
      }
    }
  }
}

TEST_CASE("Yen's K-shortest paths with lexicographical tie-breaking",
          "[core][paths]") {
  SECTION("Multi-diamond tie-breaking verification") {
    std::vector<ton::NodeInfo> diamond_nodes(6);
    for (uint16_t i = 0; i < 6; ++i) {
      diamond_nodes[i] = {.id = i,
                          .name = "D" + std::to_string(i),
                          .lon_deg = static_cast<double>(i),
                          .lat_deg = 0.0};
    }
    std::vector<ton::SpanInput> diamond_spans = {
        {.name = "E04", .u = 0, .v = 4, .override_length_km = 10.0},
        {.name = "E45", .u = 4, .v = 5, .override_length_km = 10.0},
        {.name = "E02", .u = 0, .v = 2, .override_length_km = 10.0},
        {.name = "E25", .u = 2, .v = 5, .override_length_km = 10.0},
        {.name = "E01", .u = 0, .v = 1, .override_length_km = 10.0},
        {.name = "E15", .u = 1, .v = 5, .override_length_km = 10.0},
        {.name = "E23", .u = 2, .v = 3, .override_length_km = 6.0},
        {.name = "E13", .u = 1, .v = 3, .override_length_km = 6.0},
        {.name = "E35", .u = 3, .v = 5, .override_length_km = 6.0},
    };
    const ton::Graph diamond("diamond6", diamond_nodes, diamond_spans);

    const auto routes = ton::enumerate_yen_paths(
        diamond, 0, 5, ton::CutArcs{}, ton::DistanceMetric::GeographicKm,
        /*kappa=*/0.10, /*k_max=*/10);
    REQUIRE(routes.size() == 5);
    REQUIRE(routes[0].nodes == std::vector<ton::NodeId>{0, 1, 5});
    REQUIRE(routes[1].nodes == std::vector<ton::NodeId>{0, 2, 5});
    REQUIRE(routes[2].nodes == std::vector<ton::NodeId>{0, 4, 5});
    REQUIRE(routes[3].nodes == std::vector<ton::NodeId>{0, 1, 3, 5});
    REQUIRE(routes[4].nodes == std::vector<ton::NodeId>{0, 2, 3, 5});

    const auto hop_routes = ton::enumerate_yen_paths(
        diamond, 0, 5, ton::CutArcs{}, ton::DistanceMetric::HopCount,
        /*kappa=*/0.10, /*k_max=*/10);
    REQUIRE(hop_routes.size() == 3);
    REQUIRE(hop_routes[0].nodes == std::vector<ton::NodeId>{0, 1, 5});
    REQUIRE(hop_routes[1].nodes == std::vector<ton::NodeId>{0, 2, 5});
    REQUIRE(hop_routes[2].nodes == std::vector<ton::NodeId>{0, 4, 5});
  }

  SECTION("Brute-force oracle verification on Nobel-EU") {
    const auto nobel = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
    const auto nobel_span =
        ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);

    for (const auto metric :
         {ton::DistanceMetric::GeographicKm, ton::DistanceMetric::HopCount}) {
      for (ton::NodeId s = 0; s < nobel.num_nodes(); s += 5) {
        for (ton::NodeId d = 0; d < nobel.num_nodes(); d += 3) {
          if (s == d) {
            continue;
          }
          const auto yen_healthy = ton::enumerate_yen_paths(
              nobel, s, d, ton::CutArcs{}, metric, 0.10, 10);
          const auto oracle_healthy = brute_force_simple_paths_oracle(
              nobel, s, d, ton::CutArcs{}, metric, 0.10, 10);
          REQUIRE(yen_healthy.size() == oracle_healthy.size());
          REQUIRE_FALSE(yen_healthy.empty());
          for (size_t i = 0; i < yen_healthy.size(); ++i) {
            REQUIRE(yen_healthy[i].nodes == oracle_healthy[i].nodes);
            REQUIRE(yen_healthy[i].arcs == oracle_healthy[i].arcs);
            REQUIRE(std::abs(yen_healthy[i].cost - oracle_healthy[i].cost) <=
                    1e-9);
          }
        }
      }
    }

    for (size_t idx = 0; idx < nobel_span.sim_scenarios().size(); idx += 133) {
      const auto& cut =
          nobel_span.sim_scenario(static_cast<ton::ScenarioId>(idx)).cut_arcs;
      for (const auto& [s, d] : {std::pair<ton::NodeId, ton::NodeId>{0, 27},
                                 {4, 19},
                                 {10, 16},
                                 {13, 25}}) {
        const auto yen_deg = ton::enumerate_yen_paths(
            nobel, s, d, cut, ton::DistanceMetric::GeographicKm, 0.10, 10);
        const auto oracle_deg = brute_force_simple_paths_oracle(
            nobel, s, d, cut, ton::DistanceMetric::GeographicKm, 0.10, 10);
        REQUIRE_FALSE(yen_deg.empty());
        REQUIRE(yen_deg.size() == oracle_deg.size());
        for (size_t i = 0; i < yen_deg.size(); ++i) {
          REQUIRE(yen_deg[i].nodes == oracle_deg[i].nodes);
          for (const ton::ArcId a : yen_deg[i].arcs) {
            REQUIRE_FALSE(cut.contains(a));
          }
        }
      }
    }
  }

  SECTION("Flat arena PathPool and prewarm_for_stream") {
    const auto germany = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "germany50.xml");
    const auto germany_span =
        ton::ScenarioSet::enumerate(germany, ton::CutModel::Span);

    ton::PathPool pool(germany, ton::DistanceMetric::GeographicKm, 0.10, 10);
    pool.prewarm_all_healthy();

    static_assert(std::random_access_iterator<ton::PathSlice::Iterator>);
    const ton::PathSlice sample_slice = pool.find_healthy(0, 1);
    REQUIRE_FALSE(sample_slice.empty());
    REQUIRE(std::distance(sample_slice.begin(), sample_slice.end()) ==
            static_cast<std::ptrdiff_t>(sample_slice.size()));
    REQUIRE(sample_slice.end() - sample_slice.begin() ==
            static_cast<std::ptrdiff_t>(sample_slice.size()));
    REQUIRE(*(sample_slice.begin() + 0) == sample_slice[0]);
    REQUIRE(*(sample_slice.end() - 1) == sample_slice[sample_slice.size() - 1]);

    const auto& seq_first = germany_span.seq_scenario(0);
    ton::CutArcs rev_cut;
    for (const ton::ArcId a : seq_first.stage2_cut.span()) {
      rev_cut.add_unique(a);
    }
    for (const ton::ArcId a : seq_first.stage1_cut.span()) {
      rev_cut.add_unique(a);
    }
    REQUIRE(rev_cut == seq_first.total_cut);

    const auto slice_fwd = pool.get_or_compute(seq_first.total_cut, 3, 16);
    const size_t paths_after_fwd = pool.num_paths();
    const auto slice_rev = pool.get_or_compute(rev_cut, 3, 16);
    REQUIRE(slice_fwd.first == slice_rev.first);
    REQUIRE(slice_fwd.count == slice_rev.count);
    REQUIRE(pool.num_paths() == paths_after_fwd);

    const std::vector<ton::Request> mini_stream = {
        {.id = 0, .src = 3, .dst = 16},
        {.id = 1, .src = 16, .dst = 34},
        {.id = 2, .src = 21, .dst = 3},
    };
    pool.prewarm_for_stream(mini_stream, germany_span);
    const size_t frozen_num_paths = pool.num_paths();
    for (const auto& req : mini_stream) {
      const auto h_slice = pool.find_healthy(req.src, req.dst);
      REQUIRE_FALSE(h_slice.empty());
      for (const ton::PathId pid : h_slice) {
        for (const ton::ArcId a : pool.path_arcs(pid)) {
          const ton::FailureUnitId x1 = germany_span.arc_to_unit(a);
          if (germany_span.is_active_stage1_unit(x1)) {
            REQUIRE_FALSE(
                pool.find(germany_span.unit_cut_arcs(x1), req.src, req.dst)
                    .empty());
          }
          for (const ton::ScenarioId sim_id :
               germany_span.sim_scenarios_cutting_arc(a)) {
            REQUIRE_FALSE(pool.find(germany_span.sim_scenario(sim_id).cut_arcs,
                                    req.src, req.dst)
                              .empty());
          }
        }
      }
    }
    REQUIRE(pool.num_paths() == frozen_num_paths);
  }
}
