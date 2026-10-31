#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ton/core/assert.hpp"
#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"
#include "ton/orchestration/network_state.hpp"
#include "ton/orchestration/staging_arena.hpp"
#include "ton/solvers/scratchpad.hpp"
#include "ton/solvers/solver_interface.hpp"

namespace ton {

struct AdmissionResult {
  bool admitted = false;
  Lightpath primary{};
};

// Simultaneous single-stage failure orchestrator template
// (algorithms/FAILURE_ORCHESTRATION.md Section 2 & PHASE-3.md Section 3.3).
// Instantiates SimRDF (DsaturSolver), GreedySimRDF (GreedySolver), and
// NoRerouteSimRDF (NoRerouteSolver) with static template dispatch and zero
// dynamic heap allocations inside admit_request().
template <RestorationSolver Solver>
class SimOrchestrator {
 public:
  SimOrchestrator(const Graph& graph, const ScenarioSet& scenarios,
                  const PathPool& path_pool, uint16_t max_requests = 1024,
                  Solver solver = Solver{})
      : graph_(&graph),
        scenarios_(&scenarios),
        path_pool_(&path_pool),
        solver_(std::move(solver)),
        num_arcs_(graph.num_arcs()),
        max_requests_(std::max<uint16_t>(1u, max_requests)) {
    const auto num_sim =
        static_cast<uint32_t>(scenarios.sim_scenarios().size());
    const uint16_t max_disrupted = static_cast<uint16_t>(std::min<uint32_t>(
        std::max<uint32_t>(256u, static_cast<uint32_t>(max_requests_) + 1u),
        65535u));

    scratchpad_.init(num_arcs_, max_disrupted,
                     std::max<uint16_t>(16u, path_pool.k_max()));
    staged_ = StagedAssignments::for_regime(
        scenarios, FailureRegime::Simultaneous, max_requests_);

    fail_weight_.assign(num_sim, uint64_t{0});
    rest_stamp_buf_.assign(num_sim, uint32_t{0});
    inter_visit_buf_.assign(num_sim, uint32_t{0});
    rest_scenarios_.assign(std::max<uint32_t>(1u, num_sim), INVALID_SCENARIO);
    inter_scenarios_.assign(std::max<uint32_t>(1u, num_sim), INVALID_SCENARIO);

    locked_masks_.assign(num_arcs_, EMPTY_MASK);
    disrupted_buf_.assign(max_disrupted, DisruptedRequest{});

    const size_t req_cap = static_cast<size_t>(max_requests_) + 1u;
    req_seen_stamp_.assign(req_cap, uint32_t{0});
    old_backup_lookup_.assign(req_cap, Lightpath{});
    old_backup_stamp_.assign(req_cap, uint32_t{0});
  }

  void reset() noexcept {
    fail_clock_ = 0;
    std::fill(fail_weight_.begin(), fail_weight_.end(), uint64_t{0});
    staged_.rollback();
    scratchpad_.reset_counters();
  }

  [[nodiscard]] const Graph& graph() const noexcept { return *graph_; }
  [[nodiscard]] const ScenarioSet& scenarios() const noexcept {
    return *scenarios_;
  }
  [[nodiscard]] const PathPool& path_pool() const noexcept {
    return *path_pool_;
  }
  [[nodiscard]] const SolverScratchpad& scratchpad() const noexcept {
    return scratchpad_;
  }

  [[nodiscard]] AdmissionResult admit_request(const Request& req,
                                              NetworkState& state) noexcept {
    TON_ASSERT(state.regime() == FailureRegime::Simultaneous,
               "SimOrchestrator requires Simultaneous NetworkState");
    TON_BOUNDS_CHECK(req.id < max_requests_,
                     "SimOrchestrator RequestId exceeds max_requests");

    const PathSlice primary_routes = path_pool_->find_healthy(req.src, req.dst);
    TON_ASSERT(!primary_routes.empty(),
               "Healthy primary routes missing (did caller prewarm PathPool?)");

    for (const PathId p_k : primary_routes) {
      const auto p_k_arcs = path_pool_->path_arcs(p_k);
      WavelengthMask free_prim = state.free_primary_wavelengths(p_k_arcs);
      if (free_prim == EMPTY_MASK) {
        continue;
      }

      // Identify primary-severance scenarios F_rest(p_k) once per route p_k.
      const uint32_t rest_stamp = next_rest_stamp();
      uint32_t num_rest = 0;
      for (const ArcId a : p_k_arcs) {
        for (const ScenarioId f : scenarios_->sim_scenarios_cutting_arc(a)) {
          if (rest_stamp_buf_[f] != rest_stamp) {
            rest_stamp_buf_[f] = rest_stamp;
            rest_scenarios_[num_rest++] = f;
          }
        }
      }
      sort_by_fail_weight(
          std::span<uint32_t>(rest_scenarios_.data(), num_rest));

      // Because p_k is stub-released in every F in F_rest(p_k), F_rest(p_k)
      // feasibility and staged backups are independent of w_k.
      bool rest_evaluated = false;
      StagingCheckpoint rest_checkpoint{};
      bool route_rest_infeasible = false;

      // Test working wavelengths ascending (lambda_0 -> lambda_{W-1}).
      while (free_prim != EMPTY_MASK) {
        const uint8_t w_k = lowest_wavelength(free_prim);
        free_prim &= (free_prim - 1u);

        if constexpr (!Solver::kAllowsBackupReconfiguration) {
          // Surviving-Primary Collision Gate (NO_REROUTE_BASELINE.md 3.1).
          if (state.has_interfering_state(StateDomain::Sim, p_k_arcs, w_k,
                                          rest_stamp_buf_, rest_stamp)) {
            continue;
          }
        }

        if (!rest_evaluated) {
          staged_.rollback();
          for (uint32_t i = 0; i < num_rest; ++i) {
            const ScenarioId f = rest_scenarios_[i];
            if (!solve_sim_scenario(state, req, p_k_arcs, w_k, f,
                                    /*is_rest_scenario=*/true)) {
              bump_fail_weight(f);
              staged_.rollback();
              route_rest_infeasible = true;
              break;
            }
          }
          if (route_rest_infeasible) {
            break;
          }
          rest_evaluated = true;
          rest_checkpoint = staged_.checkpoint();
        } else {
          staged_.rollback_to(rest_checkpoint);
        }

        if constexpr (Solver::kAllowsBackupReconfiguration) {
          const uint32_t visit_stamp = next_inter_visit_stamp();
          const uint32_t num_inter = state.collect_interfering_states(
              StateDomain::Sim, p_k_arcs, w_k, rest_stamp_buf_, rest_stamp,
              inter_visit_buf_, visit_stamp, inter_scenarios_);

          if (num_inter > 0u) {
            sort_by_fail_weight(
                std::span<uint32_t>(inter_scenarios_.data(), num_inter));
            bool inter_ok = true;
            for (uint32_t i = 0; i < num_inter; ++i) {
              const ScenarioId f = inter_scenarios_[i];
              if (!solve_sim_scenario(state, req, p_k_arcs, w_k, f,
                                      /*is_rest_scenario=*/false)) {
                bump_fail_weight(f);
                staged_.rollback_to(rest_checkpoint);
                inter_ok = false;
                break;
              }
            }
            if (!inter_ok) {
              continue;
            }
          }
        }

        const Lightpath committed_prim{.path_id = p_k, .wavelength = w_k};
        state.commit(req, committed_prim, staged_, *path_pool_);
        staged_.rollback();
        return AdmissionResult{.admitted = true, .primary = committed_prim};
      }
    }

    staged_.rollback();
    return AdmissionResult{.admitted = false, .primary = Lightpath{}};
  }

 private:
  [[nodiscard]] uint32_t next_rest_stamp() noexcept {
    ++cur_rest_stamp_;
    if (cur_rest_stamp_ == 0u) [[unlikely]] {
      std::fill(rest_stamp_buf_.begin(), rest_stamp_buf_.end(), uint32_t{0});
      cur_rest_stamp_ = 1u;
    }
    return cur_rest_stamp_;
  }

  [[nodiscard]] uint32_t next_inter_visit_stamp() noexcept {
    ++cur_inter_visit_stamp_;
    if (cur_inter_visit_stamp_ == 0u) [[unlikely]] {
      std::fill(inter_visit_buf_.begin(), inter_visit_buf_.end(), uint32_t{0});
      cur_inter_visit_stamp_ = 1u;
    }
    return cur_inter_visit_stamp_;
  }

  [[nodiscard]] uint32_t next_req_seen_stamp() noexcept {
    ++cur_req_seen_stamp_;
    if (cur_req_seen_stamp_ == 0u) [[unlikely]] {
      std::fill(req_seen_stamp_.begin(), req_seen_stamp_.end(), uint32_t{0});
      cur_req_seen_stamp_ = 1u;
    }
    return cur_req_seen_stamp_;
  }

  [[nodiscard]] uint32_t next_old_lp_stamp() noexcept {
    ++cur_old_lp_stamp_;
    if (cur_old_lp_stamp_ == 0u) [[unlikely]] {
      std::fill(old_backup_stamp_.begin(), old_backup_stamp_.end(),
                uint32_t{0});
      cur_old_lp_stamp_ = 1u;
    }
    return cur_old_lp_stamp_;
  }

  void bump_fail_weight(ScenarioId f) noexcept {
    fail_weight_[f] = ++fail_clock_;
  }

  void sort_by_fail_weight(std::span<uint32_t> ids) const noexcept {
    if (ids.size() <= 1u || fail_clock_ == 0u) {
      return;
    }
    std::sort(ids.begin(), ids.end(), [this](uint32_t a, uint32_t b) noexcept {
      const uint64_t wa = fail_weight_[a];
      const uint64_t wb = fail_weight_[b];
      return (wa != wb) ? (wa > wb) : (a < b);
    });
  }

  [[nodiscard]] bool solve_sim_scenario(
      const NetworkState& state, const Request& req,
      [[maybe_unused]] std::span<const ArcId> p_k_arcs,
      [[maybe_unused]] uint8_t w_k, ScenarioId f,
      [[maybe_unused]] bool is_rest_scenario) noexcept {
    const SimScenario& sc = scenarios_->sim_scenario(f);
    const auto prim_masks = state.primary_masks();
    std::copy_n(prim_masks.data(), num_arcs_, locked_masks_.data());

    const uint32_t seen_stamp = next_req_seen_stamp();

    if constexpr (!Solver::kAllowsBackupReconfiguration) {
      TON_ASSERT(is_rest_scenario,
                 "NoRerouteSimRDF only invokes solver on F_rest scenarios");
      // Stub-release severed committed primaries first, then lock existing
      // immutable backups active in scenario f.
      for (const ArcId cut_a : sc.cut_arcs.span()) {
        WavelengthMask occ = prim_masks[cut_a];
        while (occ != EMPTY_MASK) {
          const uint8_t w = lowest_wavelength(occ);
          occ &= (occ - 1u);
          const RequestId r = state.primary_owner(cut_a, w);
          TON_ASSERT(r != INVALID_REQUEST,
                     "Occupied primary bit missing owner");
          if (req_seen_stamp_[r] == seen_stamp) {
            continue;
          }
          req_seen_stamp_[r] = seen_stamp;
          const Lightpath r_prim = state.primary_lightpath(r);
          const WavelengthMask r_bit = wavelength_bit(r_prim.wavelength);
          for (const ArcId pa : path_pool_->path_arcs(r_prim.path_id)) {
            locked_masks_[pa] &= ~r_bit;
          }
        }
      }
      for (ArcId a = 0; a < num_arcs_; ++a) {
        locked_masks_[a] |= state.sim_backup_mask(f, a);
      }

      disrupted_buf_[0] = DisruptedRequest{.req = req, .old_backup = {}};
      const DegradedSubproblem sub{
          .path_pool = path_pool_,
          .cut_arcs = sc.cut_arcs,
          .domain = StateDomain::Sim,
          .state_id = f,
          .locked_masks = locked_masks_,
          .fixed_backups = {},
          .disrupted =
              std::span<const DisruptedRequest>(disrupted_buf_.data(), 1u),
      };
      return solver_.solve(sub, scratchpad_, staged_) == SolverStatus::Feasible;
    } else {
      const uint32_t lp_stamp = next_old_lp_stamp();
      for (const BackupAssignment& ba : state.sim_backups(f)) {
        old_backup_lookup_[ba.req_id] = ba.lightpath();
        old_backup_stamp_[ba.req_id] = lp_stamp;
      }

      uint16_t num_u = 0;
      for (const ArcId cut_a : sc.cut_arcs.span()) {
        WavelengthMask occ = prim_masks[cut_a];
        while (occ != EMPTY_MASK) {
          const uint8_t w = lowest_wavelength(occ);
          occ &= (occ - 1u);
          const RequestId r = state.primary_owner(cut_a, w);
          TON_ASSERT(r != INVALID_REQUEST,
                     "Occupied primary bit missing owner");
          if (req_seen_stamp_[r] == seen_stamp) {
            continue;
          }
          req_seen_stamp_[r] = seen_stamp;

          const Lightpath r_prim = state.primary_lightpath(r);
          const WavelengthMask r_bit = wavelength_bit(r_prim.wavelength);
          for (const ArcId pa : path_pool_->path_arcs(r_prim.path_id)) {
            locked_masks_[pa] &= ~r_bit;
          }

          TON_BOUNDS_CHECK(num_u < disrupted_buf_.size(),
                           "disrupted_buf_ capacity exceeded");
          disrupted_buf_[num_u++] = DisruptedRequest{
              .req = state.request(r),
              .old_backup = (old_backup_stamp_[r] == lp_stamp)
                                ? old_backup_lookup_[r]
                                : Lightpath{},
          };
        }
      }

      // Canonical ascending arrival-ID order for deterministic Psi
      // accumulation.
      for (uint16_t i = 1; i < num_u; ++i) {
        const DisruptedRequest key = disrupted_buf_[i];
        uint16_t j = i;
        while (j > 0u && disrupted_buf_[j - 1u].req.id > key.req.id) {
          disrupted_buf_[j] = disrupted_buf_[j - 1u];
          --j;
        }
        disrupted_buf_[j] = key;
      }

      if (is_rest_scenario) {
        TON_BOUNDS_CHECK(num_u < disrupted_buf_.size(),
                         "disrupted_buf_ capacity exceeded");
        disrupted_buf_[num_u++] =
            DisruptedRequest{.req = req, .old_backup = {}};
      } else {
        const WavelengthMask wk_bit = wavelength_bit(w_k);
        for (const ArcId pa : p_k_arcs) {
          locked_masks_[pa] |= wk_bit;
        }
      }

      const DegradedSubproblem sub{
          .path_pool = path_pool_,
          .cut_arcs = sc.cut_arcs,
          .domain = StateDomain::Sim,
          .state_id = f,
          .locked_masks = locked_masks_,
          .fixed_backups = {},
          .disrupted =
              std::span<const DisruptedRequest>(disrupted_buf_.data(), num_u),
      };
      return solver_.solve(sub, scratchpad_, staged_) == SolverStatus::Feasible;
    }
  }

  const Graph* graph_ = nullptr;
  const ScenarioSet* scenarios_ = nullptr;
  const PathPool* path_pool_ = nullptr;
  [[no_unique_address]] Solver solver_{};
  ArcId num_arcs_ = 0;
  uint16_t max_requests_ = 0;

  SolverScratchpad scratchpad_;
  StagedAssignments staged_;

  uint64_t fail_clock_ = 0;
  std::vector<uint64_t> fail_weight_;

  uint32_t cur_rest_stamp_ = 0;
  uint32_t cur_inter_visit_stamp_ = 0;
  uint32_t cur_req_seen_stamp_ = 0;
  uint32_t cur_old_lp_stamp_ = 0;

  std::vector<uint32_t> rest_stamp_buf_;
  std::vector<uint32_t> inter_visit_buf_;
  std::vector<uint32_t> rest_scenarios_;
  std::vector<uint32_t> inter_scenarios_;

  std::vector<WavelengthMask> locked_masks_;
  std::vector<DisruptedRequest> disrupted_buf_;
  std::vector<uint32_t> req_seen_stamp_;
  std::vector<Lightpath> old_backup_lookup_;
  std::vector<uint32_t> old_backup_stamp_;
};

}  // namespace ton
