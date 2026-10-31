#include <algorithm>
#include <random>
#include <vector>

#include "test_helpers.hpp"
#include "ton/solvers/dsatur_solver.hpp"
#include "ton/solvers/greedy_solver.hpp"
#include "ton/solvers/no_reroute_solver.hpp"
#include "ton/solvers/scratchpad.hpp"

namespace {

ton::SolverStatus brute_force_dsatur_oracle(
    const ton::DegradedSubproblem& sub,
    std::vector<ton::BackupAssignment>& out_backups) {
  out_backups.clear();
  const ton::PathPool& pool = *sub.path_pool;

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

}  // namespace

TEST_CASE(
    "Inner Restoration Solvers (NoReroute, Greedy, DSATUR, and Theorems 1-2)",
    "[solvers][dsatur]") {
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

  const auto r03 = hub_pool.find_healthy(0, 3);
  const auto r15 = hub_pool.find_healthy(1, 5);
  const auto r23 = hub_pool.find_healthy(2, 3);
  REQUIRE(r03.size() == 2);
  REQUIRE(r15.size() == 2);
  REQUIRE(r23.size() == 1);

  const ton::ArcId arc_13 = hub_g.find_arc(1, 3);
  const ton::ArcId arc_01 = hub_g.find_arc(0, 1);
  const ton::ArcId arc_02 = hub_g.find_arc(0, 2);
  const ton::ArcId arc_23 = hub_g.find_arc(2, 3);
  REQUIRE(arc_13 != ton::INVALID_ARC);
  REQUIRE(arc_01 != ton::INVALID_ARC);
  REQUIRE(arc_02 != ton::INVALID_ARC);
  REQUIRE(arc_23 != ton::INVALID_ARC);

  ton::SolverScratchpad scratch(hub_g.num_arcs(), 32, 16);
  ton::StagedAssignments staged(32, 256);
  const ton::NoRerouteSolver no_reroute;
  const ton::GreedySolver greedy;
  const ton::DsaturSolver dsatur;

  SECTION("NoRerouteSolver descending First-Fit & fixed_backups lock check") {
    std::vector<ton::WavelengthMask> locked(hub_g.num_arcs(), ton::EMPTY_MASK);
    locked[arc_13] = ton::wavelength_bit(31);
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
    REQUIRE(no_reroute.solve(nr_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
    REQUIRE(staged.num_deltas() == 1);
    REQUIRE(staged.delta_entries(0)[0].path_id == r03[0]);
    REQUIRE(staged.delta_entries(0)[0].wavelength == 30);

    const ton::BackupAssignment clashing_fb{
        .path_id = r03[0], .req_id = 7, .wavelength = 31};
    ton::DegradedSubproblem nr_clash_sub = nr_sub;
    nr_clash_sub.fixed_backups = std::span{&clashing_fb, 1};
    nr_clash_sub.disrupted = {};
    staged.rollback();
    REQUIRE(no_reroute.solve(nr_clash_sub, scratch, staged) ==
            ton::SolverStatus::Infeasible);
  }

  SECTION("Critical tie wipeout test: Greedy failure vs DSATUR success") {
    std::vector<ton::WavelengthMask> locked(hub_g.num_arcs(), ton::EMPTY_MASK);
    const ton::WavelengthMask only_w31_free =
        ton::FULL_MASK & ~ton::wavelength_bit(31);
    const ton::WavelengthMask only_w30_w31_free =
        ton::FULL_MASK & ~(ton::wavelength_bit(31) | ton::wavelength_bit(30));
    locked[arc_01] = only_w31_free;
    locked[arc_02] = only_w31_free;
    locked[arc_13] = only_w31_free;
    locked[arc_23] = only_w30_w31_free;

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
    REQUIRE(greedy.solve(clique_sub, scratch, staged) ==
            ton::SolverStatus::Infeasible);

    staged.rollback();
    scratch.reset_counters();
    REQUIRE(dsatur.solve(clique_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
    REQUIRE(scratch.theorem1_fc_checked_steps >= 1);
    REQUIRE(staged.num_deltas() == 1);
    REQUIRE(staged.delta_entries(0).size() == 3);
    for (const auto& ba : staged.delta_entries(0)) {
      if (ba.req_id == 0) {
        REQUIRE(ba.path_id == r03[1]);
        REQUIRE(ba.wavelength == 31);
      } else if (ba.req_id == 1) {
        REQUIRE(ba.path_id == r15[0]);
        REQUIRE(ba.wavelength == 31);
      } else if (ba.req_id == 2) {
        REQUIRE(ba.path_id == r23[0]);
        REQUIRE(ba.wavelength == 30);
      }
    }
  }

  SECTION("Pillar 3 Incumbency Inertia: uncongested hold vs congested yield") {
    std::vector<ton::WavelengthMask> locked(hub_g.num_arcs(), ton::EMPTY_MASK);
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
    REQUIRE(dsatur.solve(inertia_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
    REQUIRE(scratch.theorem1_bypass_steps == 2);
    REQUIRE(scratch.theorem1_fc_checked_steps == 0);
    REQUIRE(staged.delta_entries(0)[0].req_id == 0);
    REQUIRE(staged.delta_entries(0)[0].path_id == r03[0]);
    REQUIRE(staged.delta_entries(0)[0].wavelength == 31);

    locked[arc_01] = 0x00FFFFFFu;
    locked[arc_02] = 0x00FFFFFFu;
    locked[arc_35] = 0x003FFFFFu;
    locked[arc_34] = 0x003FFFFFu | ton::wavelength_bit(31);
    staged.rollback();
    REQUIRE(dsatur.solve(inertia_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
    REQUIRE(staged.delta_entries(0)[0].req_id == 0);
    REQUIRE(staged.delta_entries(0)[0].path_id == r03[1]);
    REQUIRE(staged.delta_entries(0)[0].wavelength == 31);
  }

  SECTION("Pillar 4 Active Wavelength Compaction (Lambda_active)") {
    std::vector<ton::WavelengthMask> locked(hub_g.num_arcs(), ton::EMPTY_MASK);
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
    REQUIRE(dsatur.solve(pack_sub, scratch, staged) ==
            ton::SolverStatus::Feasible);
    REQUIRE(staged.delta_entries(0).size() == 2);
    REQUIRE(staged.delta_entries(0)[1].req_id == 0);
    REQUIRE(staged.delta_entries(0)[1].wavelength == 28);
  }

  SECTION("Randomized reference oracle equivalence across 200 subproblems") {
    const auto nobel = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
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
        const ton::PathSlice cand =
            n_pool.get_or_compute(sc.cut_arcs, src, dst);
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

      REQUIRE(fast_status == oracle_status);
      if (fast_status == ton::SolverStatus::Feasible) {
        ++feasible_count;
        const auto fast_out = n_staged.delta_entries(0);
        REQUIRE(fast_out.size() == oracle_out.size());
        for (size_t i = 0; i < oracle_out.size(); ++i) {
          REQUIRE(fast_out[i] == oracle_out[i]);
        }
      } else {
        ++infeasible_count;
        REQUIRE(n_staged.empty());
      }
    }

    REQUIRE(feasible_count > 20);
    REQUIRE(infeasible_count > 20);
    REQUIRE(n_scratch.theorem1_bypass_steps > 0);
    REQUIRE(n_scratch.theorem1_fc_checked_steps > 0);
  }
}