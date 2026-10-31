#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"

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

}  // namespace

int main() {
  test_core_types_and_bitmasks();
  test_symmetric_digraph_and_sndlib_xml();
  test_dual_cut_scenarios_and_tarjan_scc();
  test_yen_path_pool_and_lexicographic_ties();
  std::cout << "[PASS] All ToN C++23 unit tests passed.\n";
  return EXIT_SUCCESS;
}
