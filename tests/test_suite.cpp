#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"
#include "ton/orchestration/network_state.hpp"
#include "ton/orchestration/sim_orchestrator.hpp"
#include "ton/orchestration/staging_arena.hpp"
#include "ton/orchestration/telemetry.hpp"
#include "ton/solvers/dsatur_solver.hpp"
#include "ton/solvers/greedy_solver.hpp"
#include "ton/solvers/no_reroute_solver.hpp"
#include "ton/solvers/scratchpad.hpp"
#include "ton/solvers/solver_interface.hpp"

namespace {

[[noreturn]] void fail_check(
    std::string_view expr, std::string_view msg,
    const std::source_location& loc = std::source_location::current()) {
  std::cerr << "[FAIL] " << loc.file_name() << ":" << loc.line() << " in "
            << loc.function_name() << "\n  Expression: " << expr;
  if (!msg.empty()) {
    std::cerr << "\n  Detail: " << msg;
  }
  std::cerr << "\n";
  std::exit(EXIT_FAILURE);
}

#define TON_CHECK(cond)      \
  do {                       \
    if (!(cond)) {           \
      fail_check(#cond, ""); \
    }                        \
  } while (false)

#define TON_CHECK_MSG(cond, msg) \
  do {                           \
    if (!(cond)) {               \
      fail_check(#cond, (msg));  \
    }                            \
  } while (false)

void test_core_types_and_bitmasks() {
  TON_BOUNDS_CHECK(true, "sanity bounds check");
  TON_ASSERT(true, "sanity assertion");
  TON_ASSUME(true);

  static_assert(sizeof(ton::WavelengthMask) == 4);
  static_assert(ton::NUM_WAVELENGTHS == 32);
  static_assert(ton::count_wavelengths(ton::FULL_MASK) == 32);
  static_assert(ton::count_wavelengths(ton::EMPTY_MASK) == 0);

  TON_CHECK(ton::lowest_wavelength(ton::FULL_MASK) == 0);
  TON_CHECK(ton::highest_wavelength(ton::FULL_MASK) == 31);
  TON_CHECK(ton::lowest_wavelength(ton::EMPTY_MASK) == ton::INVALID_WAVELENGTH);
  TON_CHECK(ton::highest_wavelength(ton::EMPTY_MASK) ==
            ton::INVALID_WAVELENGTH);

  const ton::WavelengthMask sample =
      ton::wavelength_bit(5) | ton::wavelength_bit(27);
  TON_CHECK(ton::has_wavelength(sample, 5));
  TON_CHECK(ton::has_wavelength(sample, 27));
  TON_CHECK(!ton::has_wavelength(sample, 0));
  TON_CHECK(ton::count_wavelengths(sample) == 2);
  TON_CHECK(ton::lowest_wavelength(sample) == 5);
  TON_CHECK(ton::highest_wavelength(sample) == 27);

  ton::CutArcs cut;
  cut.add_unique(19);
  cut.add_unique(3);
  cut.add_unique(19);
  cut.add_unique(11);
  TON_CHECK(cut.count == 3);
  TON_CHECK(cut.arcs[0] == 3 && cut.arcs[1] == 11 && cut.arcs[2] == 19);
  TON_CHECK(cut.contains(11));
  TON_CHECK(!cut.contains(7));

  ton::Lightpath lp;
  TON_CHECK(!lp.valid());
  lp = ton::Lightpath{.path_id = 42, .wavelength = 31};
  TON_CHECK(lp.valid());
}

std::filesystem::path repo_root() {
  return std::filesystem::path(TON_REPO_ROOT);
}

void test_symmetric_digraph_and_sndlib_xml() {
  // 1. Haversine precision & exact symmetry checks.
  TON_CHECK(ton::Graph::haversine_km(10.0, 50.0, 10.0, 50.0) == 0.0);
  const double eq_1deg = ton::Graph::haversine_km(0.0, 0.0, 1.0, 0.0);
  const double expected_eq_1deg =
      ton::EARTH_RADIUS_KM * (std::numbers::pi / 180.0);
  TON_CHECK(std::abs(eq_1deg - expected_eq_1deg) < 1e-9);

  const double d_ab = ton::Graph::haversine_km(4.51, 52.20, 13.21, 52.31);
  const double d_ba = ton::Graph::haversine_km(13.21, 52.31, 4.51, 52.20);
  TON_CHECK(d_ab == d_ba);
  TON_CHECK(d_ab > 500.0 && d_ab < 700.0);

  // 2. Programmatic synthetic graph & deterministic CSR out-arc ordering.
  std::vector<ton::NodeInfo> nodes = {
      {.id = 0, .name = "N0", .lon_deg = 0.0, .lat_deg = 0.0},
      {.id = 1, .name = "N1", .lon_deg = 1.0, .lat_deg = 0.0},
      {.id = 2, .name = "N2", .lon_deg = 1.0, .lat_deg = 1.0},
      {.id = 3, .name = "N3", .lon_deg = 0.0, .lat_deg = 1.0},
  };
  std::vector<ton::SpanInput> spans = {
      {.name = "S_0_3", .u = 0, .v = 3, .override_length_km = 100.0},
      {.name = "S_0_1", .u = 0, .v = 1, .override_length_km = 120.0},
      {.name = "S_1_2", .u = 1, .v = 2, .override_length_km = 100.0},
      {.name = "S_2_3", .u = 2, .v = 3, .override_length_km = 120.0},
  };
  const ton::Graph square("square4", nodes, spans,
                          ton::DistanceMetric::GeographicKm);
  TON_CHECK(square.num_nodes() == 4);
  TON_CHECK(square.num_spans() == 4);
  TON_CHECK(square.num_arcs() == 8);

  // Outgoing arcs from N0 must be sorted by ascending target NodeId: (0->1)
  // before (0->3).
  const auto out0 = square.out_arcs(0);
  TON_CHECK(out0.size() == 2);
  TON_CHECK(square.arc(out0[0]).dst == 1);
  TON_CHECK(square.arc(out0[1]).dst == 3);
  TON_CHECK(square.arc_cost(out0[0], ton::DistanceMetric::GeographicKm) ==
            120.0);
  TON_CHECK(square.arc_cost(out0[0], ton::DistanceMetric::HopCount) == 1.0);
  TON_CHECK(square.find_arc(0, 1) == out0[0]);
  TON_CHECK(square.find_arc(0, 2) == ton::INVALID_ARC);

  // Reject duplicate span.
  bool threw_dup = false;
  try {
    auto bad_spans = spans;
    bad_spans.push_back(
        {.name = "DUP", .u = 1, .v = 0, .override_length_km = 120.0});
    (void)ton::Graph("bad", nodes, bad_spans);
  } catch (const std::invalid_argument&) {
    threw_dup = true;
  }
  TON_CHECK(threw_dup);

  // 3. Official SNDlib Nobel-EU (28N, 41E, 82L) & Germany50 (50N, 88E, 176L).
  const auto nobel = ton::Graph::from_sndlib_xml(repo_root() / "data" /
                                                 "topologies" / "nobel-eu.xml");
  TON_CHECK(nobel.num_nodes() == 28);
  TON_CHECK(nobel.num_spans() == 41);
  TON_CHECK(nobel.num_arcs() == 82);
  TON_CHECK(nobel.find_node("Amsterdam") == 0);
  TON_CHECK(nobel.find_node("Zurich") == 27);

  const auto germany = ton::Graph::from_sndlib_xml(
      repo_root() / "data" / "topologies" / "germany50.xml");
  TON_CHECK(germany.num_nodes() == 50);
  TON_CHECK(germany.num_spans() == 88);
  TON_CHECK(germany.num_arcs() == 176);
  TON_CHECK(germany.find_node("Aachen") == 0);
  TON_CHECK(germany.find_node("Wuerzburg") == 49);

  for (const auto* g : {&nobel, &germany}) {
    for (ton::ArcId a = 0; a < g->num_arcs(); ++a) {
      const auto& arc = g->arc(a);
      TON_CHECK(arc.id == a);
      TON_CHECK(arc.src != arc.dst);
      TON_CHECK(arc.length_km > 0.0 && std::isfinite(arc.length_km));
      const auto& rev = g->arc(arc.reverse_arc);
      TON_CHECK(rev.reverse_arc == a);
      TON_CHECK(rev.src == arc.dst && rev.dst == arc.src);
      TON_CHECK(rev.span_id == arc.span_id);
      TON_CHECK(rev.length_km == arc.length_km);
      TON_CHECK(g->find_arc(arc.src, arc.dst) == a);
    }
    for (ton::NodeId u = 0; u < g->num_nodes(); ++u) {
      const auto out = g->out_arcs(u);
      TON_CHECK(!out.empty());
      for (size_t i = 1; i < out.size(); ++i) {
        TON_CHECK(g->arc(out[i - 1]).dst < g->arc(out[i]).dst);
      }
    }
  }
}

void test_dual_cut_scenarios_and_tarjan_scc() {
  // 1. Synthetic 4-cycle ("square"): cutting any 2 spans disconnects the
  // 4-cycle (0 Sim Span), while cutting 2 counter-clockwise arcs leaves the
  // clockwise 4-cycle strongly connected!
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
  TON_CHECK(ton::is_strongly_connected(square, ton::CutArcs{}));

  const auto square_span =
      ton::ScenarioSet::enumerate(square, ton::CutModel::Span);
  TON_CHECK(square_span.sim_scenarios().empty());
  TON_CHECK(square_span.seq_scenarios().empty());

  // In a bidirected 4-cycle (8 arcs, C(8,2)=28 pairs): 12 same-orientation
  // pairs + 4 single-span pairs leave the surviving digraph strongly connected
  // -> 16 Sim, 32 Seq.
  const auto square_dir =
      ton::ScenarioSet::enumerate(square, ton::CutModel::Directional);
  TON_CHECK(square_dir.sim_scenarios().size() == 16);
  TON_CHECK(square_dir.seq_scenarios().size() == 32);

  // 2. Hard-assert exact canonical benchmark scenario counts (PROBLEM.md
  // Section 3.2):
  //    Nobel-EU:  804 / 1,608 (Span),   3,289 / 6,578 (Directional)
  //    Germany50: 3,817 / 7,634 (Span), 15,378 / 30,756 (Directional)
  const auto nobel = ton::Graph::from_sndlib_xml(repo_root() / "data" /
                                                 "topologies" / "nobel-eu.xml");
  const auto germany = ton::Graph::from_sndlib_xml(
      repo_root() / "data" / "topologies" / "germany50.xml");

  const auto nobel_span =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
  TON_CHECK(nobel_span.sim_scenarios().size() == 804);
  TON_CHECK(nobel_span.seq_scenarios().size() == 1608);

  const auto nobel_dir =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Directional);
  TON_CHECK(nobel_dir.sim_scenarios().size() == 3289);
  TON_CHECK(nobel_dir.seq_scenarios().size() == 6578);

  const auto germany_span =
      ton::ScenarioSet::enumerate(germany, ton::CutModel::Span);
  TON_CHECK(germany_span.sim_scenarios().size() == 3817);
  TON_CHECK(germany_span.seq_scenarios().size() == 7634);

  const auto germany_dir =
      ton::ScenarioSet::enumerate(germany, ton::CutModel::Directional);
  TON_CHECK(germany_dir.sim_scenarios().size() == 15378);
  TON_CHECK(germany_dir.seq_scenarios().size() == 30756);

  // 3. Structural invariants across all 4 benchmark ScenarioSets:
  //    - |F_seq| == 2 * |F_sim|
  //    - Every active Stage-1 unit x1 leaves G \ L_cut(x1) strongly connected
  //    - Every coupled group G_{x1} is contiguous and maps (x1, x2) & (x2, x1)
  //    to identical sim_id
  for (const auto& [g_ptr, sc_ptr] : {
           std::pair{&nobel, &nobel_span},
           std::pair{&nobel, &nobel_dir},
           std::pair{&germany, &germany_span},
           std::pair{&germany, &germany_dir},
       }) {
    const auto& g = *g_ptr;
    const auto& sc_set = *sc_ptr;
    TON_CHECK(sc_set.seq_scenarios().size() ==
              2 * sc_set.sim_scenarios().size());

    size_t total_grouped = 0;
    for (const ton::FailureUnitId x1 : sc_set.active_stage1_units()) {
      TON_CHECK(sc_set.is_active_stage1_unit(x1));
      TON_CHECK(ton::is_strongly_connected(g, sc_set.unit_cut_arcs(x1)));
      const auto grp = sc_set.seq_group(x1);
      TON_CHECK(!grp.empty());
      total_grouped += grp.size();
      for (const auto& seq_sc : grp) {
        TON_CHECK(seq_sc.x1 == x1 && seq_sc.x2 != x1);
        const auto& sim_sc = sc_set.sim_scenario(seq_sc.sim_id);
        TON_CHECK(sim_sc.cut_arcs == seq_sc.total_cut);
        TON_CHECK((sim_sc.x1 == seq_sc.x1 && sim_sc.x2 == seq_sc.x2) ||
                  (sim_sc.x1 == seq_sc.x2 && sim_sc.x2 == seq_sc.x1));
      }
    }
    TON_CHECK(total_grouped == sc_set.seq_scenarios().size());

    // 4. Verify reverse arc indices for both simultaneous and sequential
    // Stage-2 cuts (Finding P2B-1).
    for (ton::ArcId a = 0; a < g.num_arcs(); ++a) {
      for (const ton::ScenarioId sim_id : sc_set.sim_scenarios_cutting_arc(a)) {
        const auto& sc = sc_set.sim_scenario(sim_id);
        TON_CHECK(sc.cut_arcs.contains(a));
      }
      for (const ton::ScenarioId seq_id :
           sc_set.seq_scenarios_stage2_cutting_arc(a)) {
        const auto& sc = sc_set.seq_scenario(seq_id);
        TON_CHECK(sc.stage2_cut.contains(a));
      }
    }
  }
}

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

  // First compute exact shortest simple path cost via pruned DFS.
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

void test_yen_path_pool_and_lexicographic_ties() {
  // 1. Multi-diamond equal-cost graph testing deterministic lexicographic
  // node-ID tie-breaking.
  //    Between 0 and 5, three equal-cost 2-hop paths: (0,1,5), (0,2,5), (0,4,5)
  //    [cost 20], plus two equal-cost 3-hop paths within 10% stretch: (0,1,3,5)
  //    and (0,2,3,5) [cost 22].
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
  TON_CHECK(routes.size() == 5);
  TON_CHECK((routes[0].nodes == std::vector<ton::NodeId>{0, 1, 5}));
  TON_CHECK((routes[1].nodes == std::vector<ton::NodeId>{0, 2, 5}));
  TON_CHECK((routes[2].nodes == std::vector<ton::NodeId>{0, 4, 5}));
  TON_CHECK((routes[3].nodes == std::vector<ton::NodeId>{0, 1, 3, 5}));
  TON_CHECK((routes[4].nodes == std::vector<ton::NodeId>{0, 2, 3, 5}));

  // Under HopCount with kappa=0.10, shortest=2 hops, max=2.2 hops -> strictly
  // the 3 2-hop paths in exact lexicographic order!
  const auto hop_routes = ton::enumerate_yen_paths(
      diamond, 0, 5, ton::CutArcs{}, ton::DistanceMetric::HopCount,
      /*kappa=*/0.10, /*k_max=*/10);
  TON_CHECK(hop_routes.size() == 3);
  TON_CHECK((hop_routes[0].nodes == std::vector<ton::NodeId>{0, 1, 5}));
  TON_CHECK((hop_routes[1].nodes == std::vector<ton::NodeId>{0, 2, 5}));
  TON_CHECK((hop_routes[2].nodes == std::vector<ton::NodeId>{0, 4, 5}));

  // 2. Exact agreement with brute-force all-simple-paths DFS oracle on Nobel-EU
  // across both
  //    GeographicKm and HopCount, on healthy G and dual-cut G \ L(F)!
  const auto nobel = ton::Graph::from_sndlib_xml(repo_root() / "data" /
                                                 "topologies" / "nobel-eu.xml");
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
        TON_CHECK(yen_healthy.size() == oracle_healthy.size());
        TON_CHECK(!yen_healthy.empty());
        for (size_t i = 0; i < yen_healthy.size(); ++i) {
          TON_CHECK(yen_healthy[i].nodes == oracle_healthy[i].nodes);
          TON_CHECK(yen_healthy[i].arcs == oracle_healthy[i].arcs);
          TON_CHECK(std::abs(yen_healthy[i].cost - oracle_healthy[i].cost) <=
                    1e-9);
        }
      }
    }
  }

  // Check degraded dual-cut topologies against brute-force oracle as well.
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
      TON_CHECK(!yen_deg.empty());
      TON_CHECK(yen_deg.size() == oracle_deg.size());
      for (size_t i = 0; i < yen_deg.size(); ++i) {
        TON_CHECK(yen_deg[i].nodes == oracle_deg[i].nodes);
        for (const ton::ArcId a : yen_deg[i].arcs) {
          TON_CHECK(!cut.contains(a));
        }
      }
    }
  }

  // 3. Flat contiguous PathPool arena, unordered (x1, x2)/(x2, x1)
  // deduplication, and stream
  //    prewarming on Germany50 (50N, 88E, 176L).
  const auto germany = ton::Graph::from_sndlib_xml(
      repo_root() / "data" / "topologies" / "germany50.xml");
  const auto germany_span =
      ton::ScenarioSet::enumerate(germany, ton::CutModel::Span);

  ton::PathPool pool(germany, ton::DistanceMetric::GeographicKm, 0.10, 10);
  pool.prewarm_all_healthy();

  // Test random-access iterator compliance (Finding P2A-1).
  static_assert(std::random_access_iterator<ton::PathSlice::Iterator>);
  const ton::PathSlice sample_slice = pool.find_healthy(0, 1);
  TON_CHECK(!sample_slice.empty());
  TON_CHECK(std::distance(sample_slice.begin(), sample_slice.end()) ==
            static_cast<std::ptrdiff_t>(sample_slice.size()));
  TON_CHECK(sample_slice.end() - sample_slice.begin() ==
            static_cast<std::ptrdiff_t>(sample_slice.size()));
  TON_CHECK(*(sample_slice.begin() + 0) == sample_slice[0]);
  TON_CHECK(*(sample_slice.end() - 1) == sample_slice[sample_slice.size() - 1]);
  for (ton::NodeId u = 0; u < germany.num_nodes(); ++u) {
    for (ton::NodeId v = 0; v < germany.num_nodes(); ++v) {
      if (u == v) {
        TON_CHECK(pool.find_healthy(u, v).empty());
        continue;
      }
      const auto slice = pool.find_healthy(u, v);
      TON_CHECK(!slice.empty() && slice.size() <= 10);
      const double d0 = pool.path_cost(slice[0]);
      for (uint16_t i = 0; i < slice.size(); ++i) {
        const ton::PathId pid = slice[i];
        const double ci = pool.path_cost(pid);
        TON_CHECK(ci + 1e-9 >= d0);
        TON_CHECK(ci <= 1.10 * d0 + 1e-9);
        if (i > 0) {
          TON_CHECK(ci + 1e-9 >= pool.path_cost(slice[i - 1]));
        }
        const auto p_nodes = pool.path_nodes(pid);
        const auto p_arcs = pool.path_arcs(pid);
        TON_CHECK(p_nodes.size() == p_arcs.size() + 1);
        TON_CHECK(p_nodes.front() == u && p_nodes.back() == v);
        for (size_t h = 0; h < p_arcs.size(); ++h) {
          const auto& arc = germany.arc(p_arcs[h]);
          TON_CHECK(arc.src == p_nodes[h] && arc.dst == p_nodes[h + 1]);
        }
      }
    }
  }

  // Verify unordered Stage-2 topology deduplication: (x1, x2) and (x2, x1)
  // share identical PathSlice in the flat arena without recomputing!
  const auto& seq_first = germany_span.seq_scenario(0);
  ton::CutArcs rev_cut;
  for (const ton::ArcId a : seq_first.stage2_cut.span()) {
    rev_cut.add_unique(a);
  }
  for (const ton::ArcId a : seq_first.stage1_cut.span()) {
    rev_cut.add_unique(a);
  }
  TON_CHECK(rev_cut == seq_first.total_cut);

  const auto slice_fwd = pool.get_or_compute(seq_first.total_cut, 3, 16);
  const size_t paths_after_fwd = pool.num_paths();
  const auto slice_rev = pool.get_or_compute(rev_cut, 3, 16);
  TON_CHECK(slice_fwd.first == slice_rev.first &&
            slice_fwd.count == slice_rev.count);
  TON_CHECK(pool.num_paths() == paths_after_fwd);
  for (const ton::PathId pid : slice_fwd) {
    TON_CHECK(!pool.path_intersects(pid, seq_first.total_cut));
    ton::CutArcs probe_cut;
    probe_cut.add_unique(pool.path_arcs(pid).front());
    TON_CHECK(pool.path_intersects(pid, probe_cut));
  }

  // Verify prewarm_for_stream freezes lookups for online zero-allocation
  // find().
  const std::vector<ton::Request> mini_stream = {
      {.id = 0, .src = 3, .dst = 16},
      {.id = 1, .src = 16, .dst = 34},
      {.id = 2, .src = 21, .dst = 3},
  };
  pool.prewarm_for_stream(mini_stream, germany_span);
  const size_t frozen_num_paths = pool.num_paths();
  for (const auto& req : mini_stream) {
    const auto h_slice = pool.find_healthy(req.src, req.dst);
    TON_CHECK(!h_slice.empty());
    for (const ton::PathId pid : h_slice) {
      for (const ton::ArcId a : pool.path_arcs(pid)) {
        const ton::FailureUnitId x1 = germany_span.arc_to_unit(a);
        if (germany_span.is_active_stage1_unit(x1)) {
          TON_CHECK(!pool.find(germany_span.unit_cut_arcs(x1), req.src, req.dst)
                         .empty());
        }
        for (const ton::ScenarioId sim_id :
             germany_span.sim_scenarios_cutting_arc(a)) {
          TON_CHECK(!pool.find(germany_span.sim_scenario(sim_id).cut_arcs,
                               req.src, req.dst)
                         .empty());
        }
      }
    }
  }
  TON_CHECK(pool.num_paths() == frozen_num_paths);
}

void test_network_state_staging_and_telemetry() {
  const auto nobel = ton::Graph::from_sndlib_xml(repo_root() / "data" /
                                                 "topologies" / "nobel-eu.xml");
  const auto nobel_span =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);

  ton::PathPool pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
  pool.prewarm_all_healthy();

  // 1. Simultaneous NetworkState, O(1) rollback/checkpoint, ReplaceState &
  // AppendSingle, Stub-Release overlap, and exact telemetry equations.
  ton::NetworkState sim_state(nobel, nobel_span,
                              ton::FailureRegime::Simultaneous, 64);
  auto sim_staged = ton::StagedAssignments::for_regime(
      nobel_span, ton::FailureRegime::Simultaneous, 64);

  const auto empty_metrics = ton::SpectralTelemetry::evaluate(sim_state);
  TON_CHECK(empty_metrics ==
            ton::SpectralTelemetry::evaluate_full_scan(sim_state));
  TON_CHECK(empty_metrics.h_prim_slots == 0 && empty_metrics.h_back_slots == 0);
  TON_CHECK(empty_metrics.overhead_ratio == 0.0);
  TON_CHECK(ton::SpectralTelemetry::verify_refcounts_exact(sim_state));

  const ton::Request req0{.id = 0, .src = 0, .dst = 10};
  const ton::Request req1{.id = 1, .src = 10, .dst = 27};
  const ton::PathSlice p0_slice = pool.find_healthy(req0.src, req0.dst);
  const ton::PathSlice p1_slice = pool.find_healthy(req1.src, req1.dst);
  TON_CHECK(!p0_slice.empty() && !p1_slice.empty());

  const ton::PathId p0_prim = p0_slice[0];
  const ton::PathId p1_prim = p1_slice[0];
  TON_CHECK(sim_state.is_primary_clash_free(pool.path_arcs(p0_prim), 0));

  // Find two scenarios cutting p0_prim and compute valid backup paths on G
  // \ L(F).
  const ton::ArcId first_a0 = pool.path_arcs(p0_prim).front();
  const auto cutting_p0 = nobel_span.sim_scenarios_cutting_arc(first_a0);
  TON_CHECK(cutting_p0.size() >= 2);
  const ton::ScenarioId f_a = cutting_p0[0];
  const ton::ScenarioId f_b = cutting_p0[1];

  const ton::PathId b0_fa = pool.get_or_compute(
      nobel_span.sim_scenario(f_a).cut_arcs, req0.src, req0.dst)[0];
  const ton::PathSlice b0_fb_slice = pool.get_or_compute(
      nobel_span.sim_scenario(f_b).cut_arcs, req0.src, req0.dst);
  const ton::PathId b0_fb = b0_fb_slice[0];

  // Test speculative staging, checkpoint, partial rollback, and full rollback.
  const ton::BackupAssignment ba_fa_w31{
      .path_id = b0_fa, .req_id = 0, .wavelength = 31};
  const ton::BackupAssignment ba_fb_w31{
      .path_id = b0_fb, .req_id = 0, .wavelength = 31};
  sim_staged.stage_replace(ton::StateDomain::Sim, f_a,
                           std::span{&ba_fa_w31, 1});
  const auto cp = sim_staged.checkpoint();
  sim_staged.stage_append(ton::StateDomain::Sim, f_b, ba_fb_w31);
  TON_CHECK(sim_staged.num_deltas() == 2 && sim_staged.num_entries() == 2);

  sim_staged.rollback_to(cp);
  TON_CHECK(sim_staged.num_deltas() == 1 && sim_staged.num_entries() == 1);
  sim_staged.rollback();
  TON_CHECK(sim_staged.empty() && sim_state.num_admitted() == 0);

  // Stage and commit req0 on lambda_0 with backups on lambda_31 in f_a and f_b.
  sim_staged.stage_replace(ton::StateDomain::Sim, f_a,
                           std::span{&ba_fa_w31, 1});
  sim_staged.stage_replace(ton::StateDomain::Sim, f_b,
                           std::span{&ba_fb_w31, 1});
  sim_state.commit(req0, ton::Lightpath{.path_id = p0_prim, .wavelength = 0},
                   sim_staged, pool);

  TON_CHECK(sim_state.num_admitted() == 1);
  TON_CHECK(sim_state.is_admitted(0));
  TON_CHECK(!sim_state.is_primary_clash_free(pool.path_arcs(p0_prim), 0));
  TON_CHECK(sim_state.is_primary_clash_free(pool.path_arcs(p0_prim), 1));
  TON_CHECK(sim_state.find_sim_backup(f_a, 0) ==
            (ton::Lightpath{.path_id = b0_fa, .wavelength = 31}));
  TON_CHECK(ton::SpectralTelemetry::verify_refcounts_exact(sim_state));
  TON_CHECK(ton::SpectralTelemetry::evaluate(sim_state) ==
            ton::SpectralTelemetry::evaluate_full_scan(sim_state));

  // Verify block-union conflict index finds f_a / f_b on wavelength 31 and none
  // on wavelength 30.
  std::vector<uint32_t> excl(nobel_span.sim_scenarios().size(), 0);
  std::vector<uint32_t> visit(nobel_span.sim_scenarios().size(), 0);
  std::vector<uint32_t> hits(nobel_span.sim_scenarios().size(), 0);
  TON_CHECK(sim_state.has_interfering_state(
      ton::StateDomain::Sim, pool.path_arcs(b0_fa), 31, excl, 1));
  TON_CHECK(!sim_state.has_interfering_state(
      ton::StateDomain::Sim, pool.path_arcs(b0_fa), 30, excl, 1));
  const uint32_t n_hits_before = sim_state.collect_interfering_states(
      ton::StateDomain::Sim, pool.path_arcs(b0_fa), 31, excl, 1, visit, 1,
      hits);
  TON_CHECK(n_hits_before >= 1);

  // Commit req1 on primary (p1_prim, lambda_31) if disjoint from p0_prim (since
  // p0_prim is on lambda_0, lambda_31 is 100% free in H_prim!) AND reconfigure
  // req0's backup in f_b from lambda_31 to lambda_0 (creating deliberate
  // stub-release overlap if b0_fb shares any arc with p0_prim, or pure overlap
  // between p1_prim on lambda_31 and b0_fa on lambda_31!).
  sim_staged.rollback();
  const ton::BackupAssignment ba_fb_w0{
      .path_id = b0_fb, .req_id = 0, .wavelength = 0};
  sim_staged.stage_replace(ton::StateDomain::Sim, f_b, std::span{&ba_fb_w0, 1});
  const ton::ScenarioId f_c =
      nobel_span.sim_scenarios_cutting_arc(pool.path_arcs(p1_prim).front())[0];
  const ton::PathId b1_fc = pool.get_or_compute(
      nobel_span.sim_scenario(f_c).cut_arcs, req1.src, req1.dst)[0];
  if (f_c != f_a && f_c != f_b) {
    sim_staged.stage_append(
        ton::StateDomain::Sim, f_c,
        ton::BackupAssignment{.path_id = b1_fc, .req_id = 1, .wavelength = 30});
  }
  sim_state.commit(req1, ton::Lightpath{.path_id = p1_prim, .wavelength = 31},
                   sim_staged, pool);

  TON_CHECK(sim_state.find_sim_backup(f_b, 0) ==
            (ton::Lightpath{.path_id = b0_fb, .wavelength = 0}));
  TON_CHECK(ton::SpectralTelemetry::verify_refcounts_exact(sim_state));
  const auto m_after = ton::SpectralTelemetry::evaluate(sim_state);
  const auto m_scan = ton::SpectralTelemetry::evaluate_full_scan(sim_state);
  TON_CHECK(m_after == m_scan);
  TON_CHECK(m_after.h_pure_slots + m_after.h_stub_slots ==
            m_after.h_back_slots);
  TON_CHECK(std::abs(m_after.u_total_pct -
                     (m_after.u_prim_pct + m_after.u_pure_pct)) < 1e-12);
  TON_CHECK(std::abs(m_after.overhead_ratio -
                     static_cast<double>(m_after.h_pure_slots) /
                         static_cast<double>(m_after.h_prim_slots)) < 1e-12);

  // Verify CSV schema matches PLAN.md Section 5.1 (16 exact columns).
  std::ostringstream oss;
  ton::SpectralTelemetry::write_csv_header(oss);
  ton::SpectralTelemetry::write_csv_row(oss,
                                        ton::StepRecord{
                                            .t = 1,
                                            .src = req1.src,
                                            .dst = req1.dst,
                                            .admitted = true,
                                            .latency_us = 4.25,
                                            .k_adm = sim_state.num_admitted(),
                                            .spectral = m_after,
                                        });
  const std::string csv_str = oss.str();
  TON_CHECK(csv_str.starts_with(
      "t,src,dst,admitted,latency_us,k_adm,h_prim_slots,h_back_slots,"
      "h_pure_slots,h_stub_slots,u_prim_pct,u_back_pct,u_pure_pct,"
      "u_stub_pct,u_total_pct,overhead_ratio\n1,"));

  sim_state.reset();
  TON_CHECK(sim_state.num_admitted() == 0);
  TON_CHECK(ton::SpectralTelemetry::verify_refcounts_exact(sim_state));
  TON_CHECK(ton::SpectralTelemetry::evaluate(sim_state).h_back_slots == 0);

  // 2. Full-scale Sequential NetworkState on Germany50 Directional (176 Stage-1
  // units + 30,756 Stage-2 scenarios) and < 0.5 us telemetry benchmark.
  const auto germany = ton::Graph::from_sndlib_xml(
      repo_root() / "data" / "topologies" / "germany50.xml");
  const auto germany_dir =
      ton::ScenarioSet::enumerate(germany, ton::CutModel::Directional);
  ton::PathPool g_pool(germany, ton::DistanceMetric::GeographicKm, 0.10, 10);

  ton::NetworkState seq_state(germany, germany_dir,
                              ton::FailureRegime::Sequential, 32);
  auto seq_staged = ton::StagedAssignments::for_regime(
      germany_dir, ton::FailureRegime::Sequential, 32);

  const ton::Request greq{.id = 0, .src = 3, .dst = 16};
  const ton::PathId g_prim = g_pool.get_or_compute_healthy(3, 16)[0];
  const ton::ArcId g_cut_arc = g_pool.path_arcs(g_prim).front();
  const ton::FailureUnitId gx1 = germany_dir.arc_to_unit(g_cut_arc);
  TON_CHECK(germany_dir.is_active_stage1_unit(gx1));

  const ton::PathId g_s1_bak =
      g_pool.get_or_compute(germany_dir.unit_cut_arcs(gx1), 3, 16)[0];
  const ton::BackupAssignment s1_ba{
      .path_id = g_s1_bak, .req_id = 0, .wavelength = 31};
  seq_staged.stage_replace(ton::StateDomain::SeqStage1, gx1,
                           std::span{&s1_ba, 1});

  for (const auto& seq_sc : germany_dir.seq_group(gx1)) {
    const ton::PathId s2_pid =
        g_pool.path_intersects(g_s1_bak, seq_sc.stage2_cut)
            ? g_pool.get_or_compute(seq_sc.total_cut, 3, 16)[0]
            : g_s1_bak;
    seq_staged.stage_append(
        ton::StateDomain::SeqStage2, seq_sc.id,
        ton::BackupAssignment{
            .path_id = s2_pid, .req_id = 0, .wavelength = 31});
  }

  seq_state.commit(greq, ton::Lightpath{.path_id = g_prim, .wavelength = 0},
                   seq_staged, g_pool);
  TON_CHECK(ton::SpectralTelemetry::verify_refcounts_exact(seq_state));
  TON_CHECK(ton::SpectralTelemetry::evaluate(seq_state) ==
            ton::SpectralTelemetry::evaluate_full_scan(seq_state));

  // Benchmark 2,000 L1 spectral evaluations on Germany50 (M=176, W=32).
  uint64_t sink = 0;
  constexpr int kIters = 2000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kIters; ++i) {
    const auto sm = ton::SpectralTelemetry::evaluate(seq_state);
    sink += sm.h_prim_slots + sm.h_pure_slots;
  }
  const auto t1 = std::chrono::steady_clock::now();
  TON_CHECK(sink > 0);
  const double avg_us =
      std::chrono::duration<double, std::micro>(t1 - t0).count() / kIters;
#if defined(NDEBUG)
  TON_CHECK_MSG(avg_us < 0.5,
                "Release SpectralTelemetry::evaluate exceeded 0.5 us budget");
#else
  TON_CHECK(avg_us < 50.0);
#endif
}

// Pure mathematical specification oracle for Proposed DSATUR
// (algorithms/DSATUR_HEURISTIC.md Section 3) WITHOUT Theorem 1 bypass or
// Theorem 2 incremental Psi rescaling, used to prove exact mathematical
// equivalence of our optimized zero-allocation DsaturSolver.
ton::SolverStatus brute_force_dsatur_oracle(
    const ton::DegradedSubproblem& sub,
    std::vector<ton::BackupAssignment>& out_backups) {
  out_backups.clear();
  const ton::PathPool& pool = *sub.path_pool;
  const size_t m = sub.locked_masks.size();

  std::vector<ton::WavelengthMask> s_occ(sub.locked_masks.begin(),
                                         sub.locked_masks.end());
  ton::WavelengthMask active_w = ton::EMPTY_MASK;
  for (const auto& fb : sub.fixed_backups) {
    const ton::WavelengthMask bit = ton::wavelength_bit(fb.wavelength);
    for (const ton::ArcId a : pool.path_arcs(fb.path_id)) {
      if ((s_occ[a] & bit) != 0u) {
        return ton::SolverStatus::Infeasible;
      }
      s_occ[a] |= bit;
    }
    active_w |= bit;
    out_backups.push_back(fb);
  }

  const size_t n_u = sub.disrupted.size();
  if (n_u == 0u) {
    return ton::SolverStatus::Feasible;
  }

  std::vector<ton::PathSlice> routes(n_u);
  for (size_t u = 0; u < n_u; ++u) {
    routes[u] = pool.find(sub.cut_arcs, sub.disrupted[u].req.src,
                          sub.disrupted[u].req.dst);
    if (routes[u].empty()) {
      return ton::SolverStatus::Infeasible;
    }
  }

  auto paths_intersect = [&](ton::PathId p1, ton::PathId p2) {
    for (const ton::ArcId a1 : pool.path_arcs(p1)) {
      for (const ton::ArcId a2 : pool.path_arcs(p2)) {
        if (a1 == a2) {
          return true;
        }
      }
    }
    return false;
  };

  auto common_hop_count = [&](ton::PathId p1, ton::PathId p2) {
    uint32_t common = 0;
    for (const ton::ArcId a1 : pool.path_arcs(p1)) {
      for (const ton::ArcId a2 : pool.path_arcs(p2)) {
        if (a1 == a2) {
          ++common;
        }
      }
    }
    return common;
  };

  auto path_free_mask = [&](ton::PathId pid) {
    ton::WavelengthMask used = ton::EMPTY_MASK;
    for (const ton::ArcId a : pool.path_arcs(pid)) {
      TON_CHECK(a < m);
      used |= s_occ[a];
    }
    return ~used;
  };

  std::vector<uint8_t> uncolored(n_u, 1);
  for (size_t step = 0; step < n_u; ++step) {
    std::vector<uint8_t> wl(n_u, 0);
    std::vector<uint16_t> wp(n_u, 0);

    size_t r_star = n_u;
    for (size_t u = 0; u < n_u; ++u) {
      if (uncolored[u] == 0) {
        continue;
      }
      ton::WavelengthMask uni = ton::EMPTY_MASK;
      uint16_t pairs = 0;
      for (const ton::PathId pid : routes[u]) {
        const ton::WavelengthMask fm = path_free_mask(pid);
        uni |= fm;
        pairs = static_cast<uint16_t>(pairs + ton::count_wavelengths(fm));
      }
      wl[u] = ton::count_wavelengths(uni);
      wp[u] = pairs;
      if (wl[u] == 0u) {
        return ton::SolverStatus::Infeasible;
      }
      if (r_star == n_u || wl[u] < wl[r_star] ||
          (wl[u] == wl[r_star] &&
           (wp[u] < wp[r_star] ||
            (wp[u] == wp[r_star] &&
             sub.disrupted[u].req.id < sub.disrupted[r_star].req.id)))) {
        r_star = u;
      }
    }

    bool found = false;
    ton::PathId best_p = ton::INVALID_PATH;
    uint8_t best_w = ton::INVALID_WAVELENGTH;
    ton::CandidateScore best_score{};
    const ton::Lightpath old_lp = sub.disrupted[r_star].old_backup;

    for (const ton::PathId pid : routes[r_star]) {
      const ton::WavelengthMask fm = path_free_mask(pid);
      for (int wi = 31; wi >= 0; --wi) {
        const auto w = static_cast<uint8_t>(wi);
        if (!ton::has_wavelength(fm, w)) {
          continue;
        }

        // Full 1-step Forward Checking without Theorem 1 bypass.
        bool safe = true;
        for (size_t u = 0; u < n_u; ++u) {
          if (u == r_star || uncolored[u] == 0) {
            continue;
          }
          bool u_has_valid = false;
          for (const ton::PathId pu : routes[u]) {
            const ton::WavelengthMask fmu = path_free_mask(pu);
            for (uint8_t wu = 0; wu < ton::NUM_WAVELENGTHS; ++wu) {
              if (!ton::has_wavelength(fmu, wu)) {
                continue;
              }
              if (wu != w || !paths_intersect(pid, pu)) {
                u_has_valid = true;
                break;
              }
            }
            if (u_has_valid) {
              break;
            }
          }
          if (!u_has_valid) {
            safe = false;
            break;
          }
        }
        if (!safe) {
          continue;
        }

        // Pairwise contention sum without Theorem 2 Psi table.
        double damage = 0.0;
        for (size_t u = 0; u < n_u; ++u) {
          if (u == r_star || uncolored[u] == 0) {
            continue;
          }
          uint32_t overlap_sum = 0;
          for (const ton::PathId pu : routes[u]) {
            if (ton::has_wavelength(path_free_mask(pu), w)) {
              overlap_sum += common_hop_count(pid, pu);
            }
          }
          damage +=
              static_cast<double>(overlap_sum) / static_cast<double>(wp[u]);
        }

        const double churn = (old_lp.valid() &&
                              (old_lp.path_id != pid || old_lp.wavelength != w))
                                 ? ton::DsaturSolver::kBetaInertia
                                 : 0.0;

        const ton::CandidateScore sc{
            .total_damage = damage + churn,
            .path_cost = pool.path_cost(pid),
            .is_new_channel = static_cast<uint8_t>(
                ton::has_wavelength(active_w, w) ? 0u : 1u),
            .wavelength_rank =
                static_cast<uint8_t>(ton::NUM_WAVELENGTHS - 1u - w),
        };
        if (!found || sc.is_better_than(best_score)) {
          found = true;
          best_score = sc;
          best_p = pid;
          best_w = w;
        }
      }
    }

    if (!found) {
      return ton::SolverStatus::Infeasible;
    }

    uncolored[r_star] = 0;
    active_w |= ton::wavelength_bit(best_w);
    for (const ton::ArcId a : pool.path_arcs(best_p)) {
      s_occ[a] |= ton::wavelength_bit(best_w);
    }
    out_backups.push_back(ton::BackupAssignment{
        .path_id = best_p,
        .req_id = sub.disrupted[r_star].req.id,
        .wavelength = best_w,
    });
  }
  return ton::SolverStatus::Feasible;
}

void test_inner_solvers_and_dsatur_theorems() {
  // 1. Synthetic Multi-Diamond Hub Topology testing:
  //    - NoRerouteSolver descending First-Fit & fixed-backup lock collision
  //    - GreedySolver ascending arrival order & hub-clique wipeout blindness
  //    - DsaturSolver Two-Tier MRV (-omega_lambda, -Omega, -r) & Theorem 1
  //      1-Step Forward Checking preventing critical neighbor wipeout
  //    - Pillar 3 Incumbency Inertia (beta = 1/32 = 0.03125) hold vs yield
  //    - Pillar 4 Active Compaction (Lambda_active)
  std::vector<ton::NodeInfo> hub_nodes(6);
  for (uint16_t i = 0; i < 6; ++i) {
    hub_nodes[i] = {.id = i,
                    .name = "H" + std::to_string(i),
                    .lon_deg = static_cast<double>(i),
                    .lat_deg = 0.0};
  }
  std::vector<ton::SpanInput> hub_spans = {
      {.name = "S01", .u = 0, .v = 1, .override_length_km = 10.0},
      {.name = "S13", .u = 1, .v = 3, .override_length_km = 10.0},
      {.name = "S02", .u = 0, .v = 2, .override_length_km = 10.0},
      {.name = "S23", .u = 2, .v = 3, .override_length_km = 11.0},
      {.name = "S35", .u = 3, .v = 5, .override_length_km = 10.0},
      {.name = "S34", .u = 3, .v = 4, .override_length_km = 5.5},
      {.name = "S45", .u = 4, .v = 5, .override_length_km = 5.5},
  };
  const ton::Graph hub_g("hub6", hub_nodes, hub_spans);
  ton::PathPool hub_pool(hub_g, ton::DistanceMetric::GeographicKm, 0.10, 10);
  hub_pool.prewarm_all_healthy();

  // Verify expected routes:
  // (0 -> 3): [0->1->3 (cost 20), 0->2->3 (cost 21)]
  // (1 -> 5): [1->3->5 (cost 20), 1->3->4->5 (cost 21)] — BOTH share arc
  // (1->3)! (2 -> 3): [2->3 (cost 11)]
  const auto r03 = hub_pool.find_healthy(0, 3);
  const auto r15 = hub_pool.find_healthy(1, 5);
  const auto r23 = hub_pool.find_healthy(2, 3);
  TON_CHECK(r03.size() == 2 && r15.size() == 2 && r23.size() == 1);

  const ton::ArcId arc_13 = hub_g.find_arc(1, 3);
  const ton::ArcId arc_01 = hub_g.find_arc(0, 1);
  const ton::ArcId arc_02 = hub_g.find_arc(0, 2);
  const ton::ArcId arc_23 = hub_g.find_arc(2, 3);
  TON_CHECK(arc_13 != ton::INVALID_ARC && arc_01 != ton::INVALID_ARC &&
            arc_02 != ton::INVALID_ARC && arc_23 != ton::INVALID_ARC);

  ton::SolverScratchpad scratch(hub_g.num_arcs(), 32, 16);
  ton::StagedAssignments staged(32, 256);
  const ton::NoRerouteSolver no_reroute;
  const ton::GreedySolver greedy;
  const ton::DsaturSolver dsatur;

  // 1a. NoRerouteSolver descending First-Fit & fixed_backups clash check.
  std::vector<ton::WavelengthMask> locked(hub_g.num_arcs(), ton::EMPTY_MASK);
  locked[arc_13] = ton::wavelength_bit(31);  // block w=31 on shortest 0->1->3
  const ton::DisruptedRequest nr_req{
      .req = {.id = 7, .src = 0, .dst = 3},
      .old_backup = {},
  };
  ton::DegradedSubproblem nr_sub{
      .path_pool = &hub_pool,
      .cut_arcs = {},
      .domain = ton::StateDomain::Sim,
      .state_id = 0,
      .locked_masks = locked,
      .fixed_backups = {},
      .disrupted = std::span{&nr_req, 1},
  };
  staged.rollback();
  TON_CHECK(no_reroute.solve(nr_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
  TON_CHECK(staged.num_deltas() == 1);
  // Route 0 (0->1->3, cost 20) still has w=30 free -> selects (r03[0], w=30)!
  TON_CHECK(staged.delta_entries(0)[0].path_id == r03[0]);
  TON_CHECK(staged.delta_entries(0)[0].wavelength == 30);

  // Fixed backup clash in NoReroute must return Infeasible.
  const ton::BackupAssignment clashing_fb{
      .path_id = r03[0], .req_id = 7, .wavelength = 31};
  ton::DegradedSubproblem nr_clash_sub = nr_sub;
  nr_clash_sub.fixed_backups = std::span{&clashing_fb, 1};
  nr_clash_sub.disrupted = {};
  staged.rollback();
  TON_CHECK(no_reroute.solve(nr_clash_sub, scratch, staged) ==
            ton::SolverStatus::Infeasible);

  // 1b. Critical Tie Neighbor Wipeout Test (omega_lambda == 1, Omega == 2 tie):
  // Lock all wavelengths except w=31 on 0->1, 0->2, and 1->3!
  // Add request r=2 on (2->3) with w=31..30 free (omega_lambda = 2, Omega = 2)
  // so arc (2->3) actually has NON-ZERO LCV contention on w=31!
  // - Req 0 (0->3): omega_lambda = 1 (w=31), Omega = 2 (0->1->3, 0->2->3)
  // - Req 1 (1->5): omega_lambda = 1 (w=31), Omega = 2 (1->3->5, 1->3->4->5,
  //   BOTH needing arc 1->3 on w=31!)
  // - Req 2 (2->3): omega_lambda = 2 (w=31, w=30), Omega = 2
  std::fill(locked.begin(), locked.end(), ton::EMPTY_MASK);
  const ton::WavelengthMask only_w31_free =
      ton::FULL_MASK & ~ton::wavelength_bit(31);
  const ton::WavelengthMask only_w30_w31_free =
      ton::FULL_MASK & ~(ton::wavelength_bit(31) | ton::wavelength_bit(30));
  locked[arc_01] = only_w31_free;
  locked[arc_02] = only_w31_free;
  locked[arc_13] = only_w31_free;
  locked[arc_23] = only_w30_w31_free;

  // Pass disrupted out of arrival order (2, 1, 0) to verify Greedy sorts
  // ascending (0, 1, 2) and blindly wipes out Req 1, whereas DSATUR picks Req 0
  // first via (-omega_lambda=-1, -Omega=-2, -r=0), triggers Theorem 1 FC check,
  // rejects 0->1->3 as wipeout-unsafe for Req 1, routes Req 0 via 0->2->3 on
  // w=31, Req 1 via 1->3->5 on w=31, and Req 2 via 2->3 on w=30!
  const std::vector<ton::DisruptedRequest> clique_batch = {
      {.req = {.id = 2, .src = 2, .dst = 3}, .old_backup = {}},
      {.req = {.id = 1, .src = 1, .dst = 5}, .old_backup = {}},
      {.req = {.id = 0, .src = 0, .dst = 3}, .old_backup = {}},
  };
  ton::DegradedSubproblem clique_sub{
      .path_pool = &hub_pool,
      .cut_arcs = {},
      .domain = ton::StateDomain::Sim,
      .state_id = 1,
      .locked_masks = locked,
      .fixed_backups = {},
      .disrupted = clique_batch,
  };

  staged.rollback();
  TON_CHECK(greedy.solve(clique_sub, scratch, staged) ==
            ton::SolverStatus::Infeasible);

  staged.rollback();
  scratch.reset_counters();
  TON_CHECK(dsatur.solve(clique_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
  TON_CHECK(scratch.theorem1_fc_checked_steps >= 1);
  TON_CHECK(staged.num_deltas() == 1 && staged.delta_entries(0).size() == 3);
  for (const auto& ba : staged.delta_entries(0)) {
    if (ba.req_id == 0) {
      TON_CHECK(ba.path_id == r03[1] && ba.wavelength == 31);  // 0->2->3!
    } else if (ba.req_id == 1) {
      TON_CHECK(ba.path_id == r15[0] && ba.wavelength == 31);  // 1->3->5!
    } else if (ba.req_id == 2) {
      TON_CHECK(ba.path_id == r23[0] && ba.wavelength == 30);  // 2->3 on w=30!
    }
  }

  // 1c. Pillar 3 Incumbency Inertia (beta = 1/32 = 0.03125):
  // Req 0 (0->3) holds incumbent old_backup = (0->1->3, w=31), sharing arc
  // (1->3) with neighbor Req 9 (1->5, routes 1->3->5 and 1->3->4->5, both
  // completely disjoint from Req 0's alternative route 0->2->3).
  // - Uncongested hold test: block w=31 on (3->4) so only 1 of Req 9's 2 routes
  //   uses w=31 and Omega(9) = 32 + 31 = 63 (omega_lambda(9) = 32). Restrict
  //   arc_01 & arc_02 to upper 16 wavelengths so Req 0 (omega_lambda = 16) is
  //   colored first. Damage(0->1->3, 31) = 1/63 = 0.01587 < 0.03125 => Req 0
  //   holds incumbent (0->1->3, 31).
  std::fill(locked.begin(), locked.end(), ton::EMPTY_MASK);
  const ton::ArcId arc_34 = hub_g.find_arc(3, 4);
  const ton::ArcId arc_35 = hub_g.find_arc(3, 5);
  locked[arc_01] = 0x0000FFFFu;
  locked[arc_02] = 0x0000FFFFu;
  locked[arc_34] = ton::wavelength_bit(31);
  const std::vector<ton::DisruptedRequest> inertia_batch = {
      {.req = {.id = 0, .src = 0, .dst = 3},
       .old_backup = {.path_id = r03[0], .wavelength = 31}},
      {.req = {.id = 9, .src = 1, .dst = 5}, .old_backup = {}},
  };
  ton::DegradedSubproblem inertia_sub{
      .path_pool = &hub_pool,
      .cut_arcs = {},
      .domain = ton::StateDomain::Sim,
      .state_id = 2,
      .locked_masks = locked,
      .fixed_backups = {},
      .disrupted = inertia_batch,
  };
  staged.rollback();
  scratch.reset_counters();
  TON_CHECK(dsatur.solve(inertia_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
  TON_CHECK(scratch.theorem1_bypass_steps == 2 &&
            scratch.theorem1_fc_checked_steps == 0);
  TON_CHECK(staged.delta_entries(0)[0].req_id == 0);
  TON_CHECK(staged.delta_entries(0)[0].path_id == r03[0] &&
            staged.delta_entries(0)[0].wavelength == 31);

  // - Congested yield test: restrict Req 0 to 8 wavelengths (w=24..31 on arc_01
  //   & arc_02) and Req 9 to 10 wavelengths (w=22..31 on arc_35, w=22..30 on
  //   arc_34 => omega_lambda(9) = 10 > 8, Omega(9) = 19 < 32). Now every free w
  //   on (0->1->3) inflicts Damage >= 1/19 = 0.05263 > 0.03125, whereas
  //   rerouting to disjoint (0->2->3, w=31) costs 0 + 0.03125 = 0.03125 =>
  //   Req 0 automatically yields incumbent and reroutes to (0->2->3, w=31)!
  locked[arc_01] = 0x00FFFFFFu;
  locked[arc_02] = 0x00FFFFFFu;
  locked[arc_35] = 0x003FFFFFu;
  locked[arc_34] = 0x003FFFFFu | ton::wavelength_bit(31);
  staged.rollback();
  TON_CHECK(dsatur.solve(inertia_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
  TON_CHECK(staged.delta_entries(0)[0].req_id == 0);
  TON_CHECK(staged.delta_entries(0)[0].path_id == r03[1] &&
            staged.delta_entries(0)[0].wavelength == 31);

  // 1d. Pillar 4 Active Wavelength Compaction (Lambda_active):
  // Pre-lock a fixed backup on (3->5, w=28) so Lambda_active = {28}. Disjoint
  // Req 0 (0->3) with all 32 wavelengths free and zero contention must pack
  // onto active channel w=28 (is_new_channel = 0) instead of opening w=31!
  std::fill(locked.begin(), locked.end(), ton::EMPTY_MASK);
  const ton::PathId p35 = hub_pool.find_healthy(3, 5)[0];
  const ton::BackupAssignment fixed_w28{
      .path_id = p35, .req_id = 99, .wavelength = 28};
  const ton::DisruptedRequest pack_req{.req = {.id = 0, .src = 0, .dst = 3},
                                       .old_backup = {}};
  ton::DegradedSubproblem pack_sub{
      .path_pool = &hub_pool,
      .cut_arcs = {},
      .domain = ton::StateDomain::SeqStage2,
      .state_id = 0,
      .locked_masks = locked,
      .fixed_backups = std::span{&fixed_w28, 1},
      .disrupted = std::span{&pack_req, 1},
  };
  staged.rollback();
  TON_CHECK(dsatur.solve(pack_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
  TON_CHECK(staged.delta_entries(0).size() == 2);
  TON_CHECK(staged.delta_entries(0)[1].req_id == 0);
  TON_CHECK(staged.delta_entries(0)[1].wavelength == 28);

  // 2. Randomized Brute-Force Reference Oracle Equivalence across 200 degraded
  // Nobel-EU subproblems (proving Theorem 1 bypass & Theorem 2 link-factored
  // Psi produce 100% identical outputs to the mathematical specification).
  const auto nobel = ton::Graph::from_sndlib_xml(repo_root() / "data" /
                                                 "topologies" / "nobel-eu.xml");
  const auto nobel_span =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
  ton::PathPool n_pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);

  ton::SolverScratchpad n_scratch(nobel.num_arcs(), 64, 16);
  ton::StagedAssignments n_staged(16, 256);
  std::mt19937_64 rng(20261031ULL);

  uint32_t feasible_count = 0;
  uint32_t infeasible_count = 0;

  for (int trial = 0; trial < 200; ++trial) {
    const auto& sc = nobel_span.sim_scenario(static_cast<ton::ScenarioId>(
        rng() % nobel_span.sim_scenarios().size()));

    // Generate dense locked masks so both omega_lambda >= 2 (Theorem 1 bypass)
    // and omega_lambda == 1 (critical FC checks) occur frequently.
    std::vector<ton::WavelengthMask> n_locked(nobel.num_arcs(),
                                              ton::EMPTY_MASK);
    const ton::WavelengthMask base_block =
        (trial % 3 == 0) ? 0x3FFFFFFFu : 0x0FFFFFFFu;
    for (ton::ArcId a = 0; a < nobel.num_arcs(); ++a) {
      if (!sc.cut_arcs.contains(a)) {
        n_locked[a] = base_block | static_cast<ton::WavelengthMask>(
                                       rng() & rng() & 0xFFFFFFFFu);
      }
    }

    const size_t batch_sz = 4 + (rng() % 10);
    std::vector<ton::DisruptedRequest> batch;
    batch.reserve(batch_sz);
    for (size_t i = 0; i < batch_sz; ++i) {
      const auto src = static_cast<ton::NodeId>(rng() % nobel.num_nodes());
      auto dst = static_cast<ton::NodeId>(rng() % (nobel.num_nodes() - 1));
      if (dst >= src) {
        ++dst;
      }
      const ton::PathSlice cand = n_pool.get_or_compute(sc.cut_arcs, src, dst);
      ton::Lightpath old_lp{};
      if (!cand.empty() && (rng() % 2 == 0)) {
        old_lp = ton::Lightpath{
            .path_id = cand[rng() % cand.size()],
            .wavelength = static_cast<uint8_t>(28 + (rng() % 4)),
        };
      }
      batch.push_back(ton::DisruptedRequest{
          .req = {.id = static_cast<ton::RequestId>(i * 3 + (rng() % 2)),
                  .src = src,
                  .dst = dst},
          .old_backup = old_lp,
      });
    }

    const ton::DegradedSubproblem sub{
        .path_pool = &n_pool,
        .cut_arcs = sc.cut_arcs,
        .domain = ton::StateDomain::Sim,
        .state_id = sc.id,
        .locked_masks = n_locked,
        .fixed_backups = {},
        .disrupted = batch,
    };

    n_staged.rollback();
    const ton::SolverStatus fast_status =
        dsatur.solve(sub, n_scratch, n_staged);

    std::vector<ton::BackupAssignment> oracle_out;
    const ton::SolverStatus oracle_status =
        brute_force_dsatur_oracle(sub, oracle_out);

    TON_CHECK(fast_status == oracle_status);
    if (fast_status == ton::SolverStatus::Feasible) {
      ++feasible_count;
      const auto fast_out = n_staged.delta_entries(0);
      TON_CHECK(fast_out.size() == oracle_out.size());
      for (size_t i = 0; i < oracle_out.size(); ++i) {
        TON_CHECK(fast_out[i] == oracle_out[i]);
      }
    } else {
      ++infeasible_count;
      TON_CHECK(n_staged.empty());
    }
  }

  TON_CHECK(feasible_count > 20 && infeasible_count > 20);
  TON_CHECK(n_scratch.theorem1_bypass_steps > 0);
  TON_CHECK(n_scratch.theorem1_fc_checked_steps > 0);
}

std::vector<ton::Request> load_request_stream_csv(
    const std::filesystem::path& csv_path, size_t max_count = 1000) {
  std::ifstream ifs(csv_path);
  TON_CHECK_MSG(ifs.is_open(), "Failed to open stream CSV");
  std::string line;
  TON_CHECK(static_cast<bool>(std::getline(ifs, line)));  // skip header

  std::vector<ton::Request> stream;
  stream.reserve(max_count);
  while (stream.size() < max_count && std::getline(ifs, line)) {
    if (line.empty()) {
      continue;
    }
    const size_t c1 = line.find(',');
    const size_t c2 = line.find(',', c1 + 1);
    const size_t c3 = line.find(',', c2 + 1);
    TON_CHECK(c1 != std::string::npos && c2 != std::string::npos);
    const auto t = static_cast<ton::RequestId>(std::stoul(line.substr(0, c1)));
    const auto src = static_cast<ton::NodeId>(
        std::stoul(line.substr(c1 + 1, c2 - (c1 + 1))));
    const auto dst = static_cast<ton::NodeId>(std::stoul(
        c3 == std::string::npos ? line.substr(c2 + 1)
                                : line.substr(c2 + 1, c3 - (c2 + 1))));
    stream.push_back(ton::Request{.id = t, .src = src, .dst = dst});
  }
  return stream;
}

void audit_simultaneous_state_invariants(const ton::NetworkState& state,
                                         const ton::PathPool& pool) {
  TON_CHECK(state.regime() == ton::FailureRegime::Simultaneous);
  const auto& g = state.graph();
  const auto& sc_set = state.scenarios();
  const ton::ArcId m = g.num_arcs();

  // INV-1 & INV-4 (Primary clash conservation & route admissibility).
  std::vector<ton::WavelengthMask> prim_occ(m, ton::EMPTY_MASK);
  for (const ton::RequestId r : state.admitted_ids()) {
    TON_CHECK(state.is_admitted(r));
    const ton::Request& req = state.request(r);
    const ton::Lightpath lp = state.primary_lightpath(r);
    TON_CHECK(lp.valid());

    const ton::PathSlice cand = pool.find_healthy(req.src, req.dst);
    bool in_pool = false;
    for (const ton::PathId pid : cand) {
      if (pid == lp.path_id) {
        in_pool = true;
        break;
      }
    }
    TON_CHECK(in_pool);

    const ton::WavelengthMask bit = ton::wavelength_bit(lp.wavelength);
    for (const ton::ArcId a : pool.path_arcs(lp.path_id)) {
      TON_CHECK((prim_occ[a] & bit) == 0u);
      TON_CHECK(state.primary_owner(a, lp.wavelength) == r);
      prim_occ[a] |= bit;
    }
  }
  for (ton::ArcId a = 0; a < m; ++a) {
    TON_CHECK(prim_occ[a] == state.primary_mask(a));
  }

  // INV-2, INV-3, INV-4, INV-5 across all Simultaneous scenarios F in F_sim.
  std::vector<ton::WavelengthMask> scen_occ(m, ton::EMPTY_MASK);
  std::vector<ton::WavelengthMask> back_only(m, ton::EMPTY_MASK);
  for (const auto& sc : sc_set.sim_scenarios()) {
    std::fill(scen_occ.begin(), scen_occ.end(), ton::EMPTY_MASK);
    std::fill(back_only.begin(), back_only.end(), ton::EMPTY_MASK);

    uint32_t expected_disrupted = 0;
    for (const ton::RequestId r : state.admitted_ids()) {
      const ton::Lightpath prim = state.primary_lightpath(r);
      const bool severed = pool.path_intersects(prim.path_id, sc.cut_arcs);
      if (!severed) {
        // Surviving primary slots are strictly protected (INV-5).
        TON_CHECK(!state.find_sim_backup(sc.id, r).valid());
        const ton::WavelengthMask bit = ton::wavelength_bit(prim.wavelength);
        for (const ton::ArcId a : pool.path_arcs(prim.path_id)) {
          scen_occ[a] |= bit;
        }
      } else {
        ++expected_disrupted;
      }
    }

    const auto backups = state.sim_backups(sc.id);
    TON_CHECK(backups.size() == expected_disrupted);

    for (const auto& ba : backups) {
      TON_CHECK(ba.valid() && state.is_admitted(ba.req_id));
      const ton::Request& req = state.request(ba.req_id);
      const ton::Lightpath prim = state.primary_lightpath(ba.req_id);
      TON_CHECK(pool.path_intersects(prim.path_id, sc.cut_arcs));  // INV-3
      TON_CHECK(!pool.path_intersects(ba.path_id, sc.cut_arcs));   // INV-4

      const ton::PathSlice cand = pool.find(sc.cut_arcs, req.src, req.dst);
      bool in_pool = false;
      for (const ton::PathId pid : cand) {
        if (pid == ba.path_id) {
          in_pool = true;
          break;
        }
      }
      TON_CHECK(in_pool);

      const ton::WavelengthMask bit = ton::wavelength_bit(ba.wavelength);
      for (const ton::ArcId a : pool.path_arcs(ba.path_id)) {
        TON_CHECK((scen_occ[a] & bit) ==
                  0u);  // Zero clash in operational state!
        scen_occ[a] |= bit;
        back_only[a] |= bit;
      }
    }

    for (ton::ArcId a = 0; a < m; ++a) {
      TON_CHECK(back_only[a] == state.sim_backup_mask(sc.id, a));
    }
  }

  TON_CHECK(ton::SpectralTelemetry::verify_refcounts_exact(state));
  TON_CHECK(ton::SpectralTelemetry::evaluate(state) ==
            ton::SpectralTelemetry::evaluate_full_scan(state));
}

void test_sim_orchestrator_on_nobel_eu() {
  const auto nobel = ton::Graph::from_sndlib_xml(repo_root() / "data" /
                                                 "topologies" / "nobel-eu.xml");
  const auto nobel_span =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
  const auto stream = load_request_stream_csv(
      repo_root() / "data" / "streams" / "nobel_eu_realistic_seed_01.csv", 300);
  TON_CHECK(stream.size() == 300);

  ton::PathPool pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
  pool.prewarm_for_stream(stream, nobel_span);

  // 1. Proposed SimRDF (DsaturSolver)
  ton::NetworkState sim_rdf_state(nobel, nobel_span,
                                  ton::FailureRegime::Simultaneous, 300);
  ton::SimOrchestrator<ton::DsaturSolver> sim_rdf(nobel, nobel_span, pool, 300);
  uint32_t sim_rdf_denied = 0;
  for (const auto& req : stream) {
    const auto prev_metrics = ton::SpectralTelemetry::evaluate(sim_rdf_state);
    const uint16_t prev_adm = sim_rdf_state.num_admitted();
    const auto res = sim_rdf.admit_request(req, sim_rdf_state);
    if (!res.admitted) {
      ++sim_rdf_denied;
      TON_CHECK(sim_rdf_state.num_admitted() == prev_adm);
      TON_CHECK(ton::SpectralTelemetry::evaluate(sim_rdf_state) ==
                prev_metrics);
    }
  }
  TON_CHECK(sim_rdf_state.num_admitted() > 50 && sim_rdf_denied > 0);
  audit_simultaneous_state_invariants(sim_rdf_state, pool);

  // Verify deterministic repeatability after reset().
  const uint16_t first_run_adm = sim_rdf_state.num_admitted();
  const auto first_run_metrics =
      ton::SpectralTelemetry::evaluate(sim_rdf_state);
  sim_rdf_state.reset();
  sim_rdf.reset();
  for (const auto& req : stream) {
    (void)sim_rdf.admit_request(req, sim_rdf_state);
  }
  TON_CHECK(sim_rdf_state.num_admitted() == first_run_adm);
  TON_CHECK(ton::SpectralTelemetry::evaluate(sim_rdf_state) ==
            first_run_metrics);

  // 2. GreedySimRDF (GreedySolver)
  ton::NetworkState greedy_sim_state(nobel, nobel_span,
                                     ton::FailureRegime::Simultaneous, 300);
  ton::SimOrchestrator<ton::GreedySolver> greedy_sim(nobel, nobel_span, pool,
                                                     300);
  for (const auto& req : stream) {
    (void)greedy_sim.admit_request(req, greedy_sim_state);
  }
  TON_CHECK(greedy_sim_state.num_admitted() > 50);
  audit_simultaneous_state_invariants(greedy_sim_state, pool);

  // 3. NoRerouteSimRDF (NoRerouteSolver) + strict immutability verification!
  ton::NetworkState nr_sim_state(nobel, nobel_span,
                                 ton::FailureRegime::Simultaneous, 300);
  ton::SimOrchestrator<ton::NoRerouteSolver> nr_sim(nobel, nobel_span, pool,
                                                    300);

  struct FrozenBackupRecord {
    ton::ScenarioId f;
    ton::RequestId r;
    ton::Lightpath lp;
  };
  std::vector<FrozenBackupRecord> frozen_records;

  for (const auto& req : stream) {
    const auto res = nr_sim.admit_request(req, nr_sim_state);
    if (res.admitted) {
      for (const auto& sc : nobel_span.sim_scenarios()) {
        if (pool.path_intersects(res.primary.path_id, sc.cut_arcs)) {
          const ton::Lightpath lp = nr_sim_state.find_sim_backup(sc.id, req.id);
          TON_CHECK(lp.valid());
          frozen_records.push_back(
              FrozenBackupRecord{.f = sc.id, .r = req.id, .lp = lp});
        }
      }
    }
  }
  TON_CHECK(nr_sim_state.num_admitted() > 40);
  // Every dynamic-reconfiguration algorithm should match or outperform
  // immutable No-Reroute on hub-skewed realistic Nobel-EU traffic.
  TON_CHECK(sim_rdf_state.num_admitted() >= nr_sim_state.num_admitted());
  for (const auto& rec : frozen_records) {
    TON_CHECK(nr_sim_state.find_sim_backup(rec.f, rec.r) == rec.lp);
  }
  audit_simultaneous_state_invariants(nr_sim_state, pool);
}

}  // namespace

int main() {
  test_core_types_and_bitmasks();
  test_symmetric_digraph_and_sndlib_xml();
  test_dual_cut_scenarios_and_tarjan_scc();
  test_yen_path_pool_and_lexicographic_ties();
  test_network_state_staging_and_telemetry();
  test_inner_solvers_and_dsatur_theorems();
  test_sim_orchestrator_on_nobel_eu();
  std::cout << "[PASS] All ToN C++23 unit tests passed.\n";
  return EXIT_SUCCESS;
}
