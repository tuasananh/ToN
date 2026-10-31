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
#include "ton/orchestration/sim_orchestrator.hpp"
#include "ton/orchestration/staging_arena.hpp"
#include "ton/solvers/scratchpad.hpp"
#include "ton/solvers/solver_interface.hpp"

namespace ton {

// Sequential two-stage coupled failure orchestrator template
// (algorithms/FAILURE_ORCHESTRATION.md Section 3 & PHASE-3.md Section 3.4).
// Enforces strict single-pass Phase A Stage-1 resolution across E^aff_s1 prior
// to Phase B Stage-2 resolution subject to locked surviving Stage-1 backups
// A_fixed(F), severed Stage-1 backups U^(2,broken)_F, and second-cut primary
// severances U^(2,prim)_F.
template <RestorationSolver Solver>
class SeqOrchestrator {
 public:
  SeqOrchestrator(const Graph& graph, const ScenarioSet& scenarios,
                  const PathPool& path_pool, uint16_t max_requests = 1024,
                  Solver solver = Solver{})
      : graph_(&graph),
        scenarios_(&scenarios),
        path_pool_(&path_pool),
        solver_(std::move(solver)),
        num_arcs_(graph.num_arcs()),
        max_requests_(std::max<uint16_t>(1u, max_requests)) {
    const uint32_t num_s1 = scenarios.num_units();
    const auto num_s2 = static_cast<uint32_t>(scenarios.seq_scenarios().size());
    const uint16_t max_disrupted = static_cast<uint16_t>(std::min<uint32_t>(
        std::max<uint32_t>(256u, static_cast<uint32_t>(max_requests_) + 1u),
        65535u));

    scratchpad_.init(num_arcs_, max_disrupted,
                     std::max<uint16_t>(16u, path_pool.k_max()));
    staged_ = StagedAssignments::for_regime(
        scenarios, FailureRegime::Sequential, max_requests_);

    s1_fail_weight_.assign(std::max<uint32_t>(1u, num_s1), uint64_t{0});
    s2_fail_weight_.assign(std::max<uint32_t>(1u, num_s2), uint64_t{0});

    rest_s1_stamp_.assign(std::max<uint32_t>(1u, num_s1), uint32_t{0});
    aff_s1_stamp_.assign(std::max<uint32_t>(1u, num_s1), uint32_t{0});
    s1_visit_buf_.assign(std::max<uint32_t>(1u, num_s1), uint32_t{0});
    staged_s1_delta_.assign(std::max<uint32_t>(1u, num_s1), uint32_t{0});

    rest_s2_only_stamp_.assign(std::max<uint32_t>(1u, num_s2), uint32_t{0});
    total_rest_s2_stamp_.assign(std::max<uint32_t>(1u, num_s2), uint32_t{0});
    s2_visit_buf_.assign(std::max<uint32_t>(1u, num_s2), uint32_t{0});

    rest_s1_units_.assign(std::max<uint32_t>(1u, num_s1), 0u);
    inter_s1_units_.assign(std::max<uint32_t>(1u, num_s1), 0u);
    rest_s2_only_scenarios_.assign(std::max<uint32_t>(1u, num_s2),
                                   INVALID_SCENARIO);
    inter_s2_scenarios_.assign(std::max<uint32_t>(1u, num_s2),
                               INVALID_SCENARIO);
    eval_s2_scenarios_.assign(std::max<uint32_t>(1u, num_s2), INVALID_SCENARIO);

    locked_masks_.assign(num_arcs_, EMPTY_MASK);
    fixed_buf_.assign(max_disrupted, BackupAssignment{});
    disrupted_buf_.assign(max_disrupted, DisruptedRequest{});

    const size_t req_cap = static_cast<size_t>(max_requests_) + 1u;
    req_seen_stamp_.assign(req_cap, uint32_t{0});
    old_backup_lookup_.assign(req_cap, Lightpath{});
    old_backup_stamp_.assign(req_cap, uint32_t{0});
  }

  void reset() noexcept {
    fail_clock_ = 0;
    std::fill(s1_fail_weight_.begin(), s1_fail_weight_.end(), uint64_t{0});
    std::fill(s2_fail_weight_.begin(), s2_fail_weight_.end(), uint64_t{0});
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
    TON_ASSERT(state.regime() == FailureRegime::Sequential,
               "SeqOrchestrator requires Sequential NetworkState");
    TON_BOUNDS_CHECK(req.id < max_requests_,
                     "SeqOrchestrator RequestId exceeds max_requests");

    const PathSlice primary_routes = path_pool_->find_healthy(req.src, req.dst);
    TON_ASSERT(!primary_routes.empty(),
               "Healthy primary routes missing (did caller prewarm PathPool?)");

    for (const PathId p_k : primary_routes) {
      const auto p_k_arcs = path_pool_->path_arcs(p_k);
      WavelengthMask free_prim = state.free_primary_wavelengths(p_k_arcs);
      if (free_prim == EMPTY_MASK) {
        continue;
      }

      // Compute route-level severance sets once per primary route p_k.
      const uint32_t route_stamp = next_route_stamp();
      uint32_t num_rest_s1 = 0;
      for (const ArcId a : p_k_arcs) {
        const FailureUnitId x1 = scenarios_->arc_to_unit(a);
        if (x1 != INVALID_FAILURE_UNIT &&
            scenarios_->is_active_stage1_unit(x1) &&
            rest_s1_stamp_[x1] != route_stamp) {
          rest_s1_stamp_[x1] = route_stamp;
          rest_s1_units_[num_rest_s1++] = x1;
          for (const SeqScenario& seq_sc : scenarios_->seq_group(x1)) {
            total_rest_s2_stamp_[seq_sc.id] = route_stamp;
          }
        }
      }

      uint32_t num_rest_s2_only = 0;
      for (const ArcId a : p_k_arcs) {
        for (const ScenarioId f :
             scenarios_->seq_scenarios_stage2_cutting_arc(a)) {
          if (rest_s2_only_stamp_[f] != route_stamp) {
            rest_s2_only_stamp_[f] = route_stamp;
            total_rest_s2_stamp_[f] = route_stamp;
            const FailureUnitId x1 = scenarios_->seq_scenario(f).x1;
            if (rest_s1_stamp_[x1] != route_stamp) {
              rest_s2_only_scenarios_[num_rest_s2_only++] = f;
            }
          }
        }
      }

      sort_s1_by_fail_weight(
          std::span<uint32_t>(rest_s1_units_.data(), num_rest_s1));

      bool rest_s1_evaluated = false;
      StagingCheckpoint cp_rest_s1{};
      bool route_infeasible = false;

      // Test working wavelengths ascending (lambda_0 -> lambda_{W-1}).
      while (free_prim != EMPTY_MASK) {
        const uint8_t w_k = lowest_wavelength(free_prim);
        free_prim &= (free_prim - 1u);

        if constexpr (!Solver::kAllowsBackupReconfiguration) {
          // Surviving-Primary Collision Gate (NO_REROUTE_BASELINE.md 3.2):
          // Reject immediately if surviving p_k collides with any immutable
          // Stage-1 or Stage-2 backup.
          if (state.has_interfering_state(StateDomain::SeqStage1, p_k_arcs, w_k,
                                          rest_s1_stamp_, route_stamp) ||
              state.has_interfering_state(StateDomain::SeqStage2, p_k_arcs, w_k,
                                          total_rest_s2_stamp_, route_stamp)) {
            continue;
          }
        }

        // Phase A (Part 1): Single-pass Stage-1 resolution for severed x1
        // (independent of w_k).
        if (!rest_s1_evaluated) {
          staged_.rollback();
          for (uint32_t i = 0; i < num_rest_s1; ++i) {
            const auto x1 = static_cast<FailureUnitId>(rest_s1_units_[i]);
            uint32_t delta_idx = 0;
            if (!solve_seq_stage1_unit(state, req, p_k_arcs, w_k, x1,
                                       /*is_rest_unit=*/true, delta_idx)) {
              bump_s1_fail_weight(x1);
              staged_.rollback();
              route_infeasible = true;
              break;
            }
            staged_s1_delta_[x1] = delta_idx;
          }
          if (route_infeasible) {
            break;
          }
          rest_s1_evaluated = true;
          cp_rest_s1 = staged_.checkpoint();
        } else {
          staged_.rollback_to(cp_rest_s1);
        }

        const uint32_t trial_stamp = next_trial_stamp();
        for (uint32_t i = 0; i < num_rest_s1; ++i) {
          aff_s1_stamp_[rest_s1_units_[i]] = trial_stamp;
        }

        // Phase A (Part 2): Single-pass Stage-1 resolution for backup-collision
        // units E^inter_s1(p_k, w_k) BEFORE entering any Stage-2 scenario.
        uint32_t num_inter_s1 = 0;
        if constexpr (Solver::kAllowsBackupReconfiguration) {
          const uint32_t s1_visit = next_s1_visit_stamp();
          const uint32_t raw_inter_s1 = state.collect_interfering_states(
              StateDomain::SeqStage1, p_k_arcs, w_k, rest_s1_stamp_,
              route_stamp, s1_visit_buf_, s1_visit, inter_s1_units_);
          for (uint32_t i = 0; i < raw_inter_s1; ++i) {
            const auto x1 = static_cast<FailureUnitId>(inter_s1_units_[i]);
            if (scenarios_->is_active_stage1_unit(x1)) {
              inter_s1_units_[num_inter_s1++] = x1;
              aff_s1_stamp_[x1] = trial_stamp;
            }
          }

          if (num_inter_s1 > 0u) {
            sort_s1_by_fail_weight(
                std::span<uint32_t>(inter_s1_units_.data(), num_inter_s1));
            bool inter_s1_ok = true;
            for (uint32_t i = 0; i < num_inter_s1; ++i) {
              const auto x1 = static_cast<FailureUnitId>(inter_s1_units_[i]);
              uint32_t delta_idx = 0;
              if (!solve_seq_stage1_unit(state, req, p_k_arcs, w_k, x1,
                                         /*is_rest_unit=*/false, delta_idx)) {
                bump_s1_fail_weight(x1);
                staged_.rollback_to(cp_rest_s1);
                inter_s1_ok = false;
                break;
              }
              staged_s1_delta_[x1] = delta_idx;
            }
            if (!inter_s1_ok) {
              continue;
            }
          }
        }

        // Phase B: Stage-2 resolution across all coupled groups G_{x1} (for
        // x1 in E^aff_s1) and directly affected scenarios F^aff_s2 (for
        // x1 not in E^aff_s1).
        uint32_t num_eval_s2 = 0;
        for (uint32_t i = 0; i < num_rest_s1; ++i) {
          const auto x1 = static_cast<FailureUnitId>(rest_s1_units_[i]);
          for (const SeqScenario& seq_sc : scenarios_->seq_group(x1)) {
            eval_s2_scenarios_[num_eval_s2++] = seq_sc.id;
          }
        }
        for (uint32_t i = 0; i < num_inter_s1; ++i) {
          const auto x1 = static_cast<FailureUnitId>(inter_s1_units_[i]);
          for (const SeqScenario& seq_sc : scenarios_->seq_group(x1)) {
            eval_s2_scenarios_[num_eval_s2++] = seq_sc.id;
          }
        }
        for (uint32_t i = 0; i < num_rest_s2_only; ++i) {
          const ScenarioId f = rest_s2_only_scenarios_[i];
          if (aff_s1_stamp_[scenarios_->seq_scenario(f).x1] != trial_stamp) {
            eval_s2_scenarios_[num_eval_s2++] = f;
          }
        }
        if constexpr (Solver::kAllowsBackupReconfiguration) {
          const uint32_t s2_visit = next_s2_visit_stamp();
          const uint32_t raw_inter_s2 = state.collect_interfering_states(
              StateDomain::SeqStage2, p_k_arcs, w_k, total_rest_s2_stamp_,
              route_stamp, s2_visit_buf_, s2_visit, inter_s2_scenarios_);
          for (uint32_t i = 0; i < raw_inter_s2; ++i) {
            const ScenarioId f = inter_s2_scenarios_[i];
            if (aff_s1_stamp_[scenarios_->seq_scenario(f).x1] != trial_stamp) {
              eval_s2_scenarios_[num_eval_s2++] = f;
            }
          }
        }

        sort_s2_by_fail_weight(
            std::span<uint32_t>(eval_s2_scenarios_.data(), num_eval_s2));

        bool phase_b_ok = true;
        for (uint32_t i = 0; i < num_eval_s2; ++i) {
          const ScenarioId f = eval_s2_scenarios_[i];
          if (!solve_seq_stage2_scenario(state, req, p_k_arcs, w_k, f,
                                         route_stamp, trial_stamp)) {
            bump_s2_fail_weight(f);
            phase_b_ok = false;
            if (!Solver::kAllowsBackupReconfiguration ||
                rest_s1_stamp_[scenarios_->seq_scenario(f).x1] == route_stamp) {
              // Under NoRerouteSeqRDF, or whenever x1 severs p_k, Stage-2
              // feasibility is 100% independent of w_k -> abort route p_k!
              staged_.rollback();
              route_infeasible = true;
            } else {
              staged_.rollback_to(cp_rest_s1);
            }
            break;
          }
        }

        if (route_infeasible) {
          break;
        }
        if (!phase_b_ok) {
          continue;
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
  [[nodiscard]] uint32_t next_route_stamp() noexcept {
    ++cur_route_stamp_;
    if (cur_route_stamp_ == 0u) [[unlikely]] {
      std::fill(rest_s1_stamp_.begin(), rest_s1_stamp_.end(), uint32_t{0});
      std::fill(rest_s2_only_stamp_.begin(), rest_s2_only_stamp_.end(),
                uint32_t{0});
      std::fill(total_rest_s2_stamp_.begin(), total_rest_s2_stamp_.end(),
                uint32_t{0});
      cur_route_stamp_ = 1u;
    }
    return cur_route_stamp_;
  }

  [[nodiscard]] uint32_t next_trial_stamp() noexcept {
    ++cur_trial_stamp_;
    if (cur_trial_stamp_ == 0u) [[unlikely]] {
      std::fill(aff_s1_stamp_.begin(), aff_s1_stamp_.end(), uint32_t{0});
      cur_trial_stamp_ = 1u;
    }
    return cur_trial_stamp_;
  }

  [[nodiscard]] uint32_t next_s1_visit_stamp() noexcept {
    ++cur_s1_visit_stamp_;
    if (cur_s1_visit_stamp_ == 0u) [[unlikely]] {
      std::fill(s1_visit_buf_.begin(), s1_visit_buf_.end(), uint32_t{0});
      cur_s1_visit_stamp_ = 1u;
    }
    return cur_s1_visit_stamp_;
  }

  [[nodiscard]] uint32_t next_s2_visit_stamp() noexcept {
    ++cur_s2_visit_stamp_;
    if (cur_s2_visit_stamp_ == 0u) [[unlikely]] {
      std::fill(s2_visit_buf_.begin(), s2_visit_buf_.end(), uint32_t{0});
      cur_s2_visit_stamp_ = 1u;
    }
    return cur_s2_visit_stamp_;
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

  void bump_s1_fail_weight(FailureUnitId x1) noexcept {
    s1_fail_weight_[x1] = ++fail_clock_;
  }

  void bump_s2_fail_weight(ScenarioId f) noexcept {
    s2_fail_weight_[f] = ++fail_clock_;
  }

  void sort_s1_by_fail_weight(std::span<uint32_t> ids) const noexcept {
    if (ids.size() <= 1u || fail_clock_ == 0u) {
      return;
    }
    std::sort(ids.begin(), ids.end(), [this](uint32_t a, uint32_t b) noexcept {
      const uint64_t wa = s1_fail_weight_[a];
      const uint64_t wb = s1_fail_weight_[b];
      return (wa != wb) ? (wa > wb) : (a < b);
    });
  }

  void sort_s2_by_fail_weight(std::span<uint32_t> ids) const noexcept {
    if (ids.size() <= 1u || fail_clock_ == 0u) {
      return;
    }
    std::sort(ids.begin(), ids.end(), [this](uint32_t a, uint32_t b) noexcept {
      const uint64_t wa = s2_fail_weight_[a];
      const uint64_t wb = s2_fail_weight_[b];
      return (wa != wb) ? (wa > wb) : (a < b);
    });
  }

  static void sort_disrupted_by_id(std::span<DisruptedRequest> items) noexcept {
    for (size_t i = 1; i < items.size(); ++i) {
      const DisruptedRequest key = items[i];
      size_t j = i;
      while (j > 0u && items[j - 1u].req.id > key.req.id) {
        items[j] = items[j - 1u];
        --j;
      }
      items[j] = key;
    }
  }

  [[nodiscard]] bool solve_seq_stage1_unit(
      const NetworkState& state, const Request& req,
      [[maybe_unused]] std::span<const ArcId> p_k_arcs,
      [[maybe_unused]] uint8_t w_k, FailureUnitId x1,
      [[maybe_unused]] bool is_rest_unit, uint32_t& out_delta_idx) noexcept {
    const CutArcs& s1_cut = scenarios_->unit_cut_arcs(x1);
    const auto prim_masks = state.primary_masks();
    std::copy_n(prim_masks.data(), num_arcs_, locked_masks_.data());

    const uint32_t seen_stamp = next_req_seen_stamp();

    if constexpr (!Solver::kAllowsBackupReconfiguration) {
      TON_ASSERT(is_rest_unit,
                 "NoRerouteSeqRDF Stage 1 only solves severed first cuts");
      for (const ArcId cut_a : s1_cut.span()) {
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
        locked_masks_[a] |= state.seq_stage1_backup_mask(x1, a);
      }

      disrupted_buf_[0] = DisruptedRequest{.req = req, .old_backup = {}};
      const DegradedSubproblem sub{
          .path_pool = path_pool_,
          .cut_arcs = s1_cut,
          .domain = StateDomain::SeqStage1,
          .state_id = x1,
          .locked_masks = locked_masks_,
          .fixed_backups = {},
          .disrupted =
              std::span<const DisruptedRequest>(disrupted_buf_.data(), 1u),
      };
      if (solver_.solve(sub, scratchpad_, staged_) != SolverStatus::Feasible) {
        return false;
      }
      out_delta_idx = staged_.num_deltas() - 1u;
      return true;
    } else {
      const uint32_t lp_stamp = next_old_lp_stamp();
      for (const BackupAssignment& ba : state.seq_stage1_backups(x1)) {
        old_backup_lookup_[ba.req_id] = ba.lightpath();
        old_backup_stamp_[ba.req_id] = lp_stamp;
      }

      uint16_t num_u = 0;
      for (const ArcId cut_a : s1_cut.span()) {
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

      sort_disrupted_by_id(
          std::span<DisruptedRequest>(disrupted_buf_.data(), num_u));

      if (is_rest_unit) {
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
          .cut_arcs = s1_cut,
          .domain = StateDomain::SeqStage1,
          .state_id = x1,
          .locked_masks = locked_masks_,
          .fixed_backups = {},
          .disrupted =
              std::span<const DisruptedRequest>(disrupted_buf_.data(), num_u),
      };
      if (solver_.solve(sub, scratchpad_, staged_) != SolverStatus::Feasible) {
        return false;
      }
      out_delta_idx = staged_.num_deltas() - 1u;
      return true;
    }
  }

  [[nodiscard]] bool solve_seq_stage2_scenario(
      const NetworkState& state, const Request& req,
      [[maybe_unused]] std::span<const ArcId> p_k_arcs,
      [[maybe_unused]] uint8_t w_k, ScenarioId f, uint32_t route_stamp,
      [[maybe_unused]] uint32_t trial_stamp) noexcept {
    const SeqScenario& sc = scenarios_->seq_scenario(f);
    const auto prim_masks = state.primary_masks();
    std::copy_n(prim_masks.data(), num_arcs_, locked_masks_.data());

    const uint32_t seen_stamp = next_req_seen_stamp();
    const bool k_cut_by_x1 = (rest_s1_stamp_[sc.x1] == route_stamp);
    const bool k_cut_by_x2 = (rest_s2_only_stamp_[f] == route_stamp);

    if constexpr (!Solver::kAllowsBackupReconfiguration) {
      TON_ASSERT(k_cut_by_x1 || k_cut_by_x2,
                 "NoRerouteSeqRDF Stage 2 only evaluates severed k scenarios");
      for (const ArcId cut_a : sc.total_cut.span()) {
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
        locked_masks_[a] |= state.seq_stage2_backup_mask(f, a);
      }

      if (k_cut_by_x1) {
        const auto s1_entries = staged_.delta_entries(staged_s1_delta_[sc.x1]);
        TON_ASSERT(s1_entries.size() == 1u && s1_entries[0].req_id == req.id,
                   "Expected single staged Stage-1 backup for k");
        const BackupAssignment s1_k = s1_entries[0];
        if (!path_pool_->path_intersects(s1_k.path_id, sc.stage2_cut)) {
          // Stage-1 backup of k survives x2 -> locked in Stage 2; verify zero
          // collision with S_locked^(2)(F).
          fixed_buf_[0] = s1_k;
          const DegradedSubproblem sub{
              .path_pool = path_pool_,
              .cut_arcs = sc.total_cut,
              .domain = StateDomain::SeqStage2,
              .state_id = f,
              .locked_masks = locked_masks_,
              .fixed_backups =
                  std::span<const BackupAssignment>(fixed_buf_.data(), 1u),
              .disrupted = {},
          };
          return solver_.solve(sub, scratchpad_, staged_) ==
                 SolverStatus::Feasible;
        }
      }

      // Either s1_k was severed by x2, or p_k survived x1 and was cut by x2.
      disrupted_buf_[0] = DisruptedRequest{.req = req, .old_backup = {}};
      const DegradedSubproblem sub{
          .path_pool = path_pool_,
          .cut_arcs = sc.total_cut,
          .domain = StateDomain::SeqStage2,
          .state_id = f,
          .locked_masks = locked_masks_,
          .fixed_backups = {},
          .disrupted =
              std::span<const DisruptedRequest>(disrupted_buf_.data(), 1u),
      };
      return solver_.solve(sub, scratchpad_, staged_) == SolverStatus::Feasible;
    } else {
      const uint32_t lp_stamp = next_old_lp_stamp();
      for (const BackupAssignment& ba : state.seq_stage2_backups(f)) {
        old_backup_lookup_[ba.req_id] = ba.lightpath();
        old_backup_stamp_[ba.req_id] = lp_stamp;
      }

      // 1. Stub-release committed primaries severed in Stage 1 (they will be
      //    partitioned into A_fixed(F) or U^(2,broken)_F from s1_backups).
      for (const ArcId cut_a : sc.stage1_cut.span()) {
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

      // 2. Stub-release committed primaries surviving x1 and newly severed by
      //    x2, placing them into U^(2,prim)_F.
      uint16_t num_u = 0;
      for (const ArcId cut_a : sc.stage2_cut.span()) {
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

      // 3. Partition Stage-1 backups U^(1)_{x1} into locked surviving backups
      //    A_fixed(F) and double-disrupted requests U^(2,broken)_F.
      const std::span<const BackupAssignment> s1_backups =
          (aff_s1_stamp_[sc.x1] == trial_stamp)
              ? staged_.delta_entries(staged_s1_delta_[sc.x1])
              : state.seq_stage1_backups(sc.x1);

      uint16_t num_fixed = 0;
      for (const BackupAssignment& s1_ba : s1_backups) {
        if (!path_pool_->path_intersects(s1_ba.path_id, sc.stage2_cut)) {
          TON_BOUNDS_CHECK(num_fixed < fixed_buf_.size(),
                           "fixed_buf_ capacity exceeded");
          fixed_buf_[num_fixed++] = s1_ba;
        } else {
          TON_BOUNDS_CHECK(num_u < disrupted_buf_.size(),
                           "disrupted_buf_ capacity exceeded");
          const bool is_k = (s1_ba.req_id == req.id);
          disrupted_buf_[num_u++] = DisruptedRequest{
              .req = is_k ? req : state.request(s1_ba.req_id),
              .old_backup =
                  (!is_k && old_backup_stamp_[s1_ba.req_id] == lp_stamp)
                      ? old_backup_lookup_[s1_ba.req_id]
                      : Lightpath{},
          };
        }
      }

      // 4. Handle incoming request k when p_k survived Stage-1 cut x1.
      if (!k_cut_by_x1) {
        if (k_cut_by_x2) {
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
      }

      sort_disrupted_by_id(
          std::span<DisruptedRequest>(disrupted_buf_.data(), num_u));

      const DegradedSubproblem sub{
          .path_pool = path_pool_,
          .cut_arcs = sc.total_cut,
          .domain = StateDomain::SeqStage2,
          .state_id = f,
          .locked_masks = locked_masks_,
          .fixed_backups =
              std::span<const BackupAssignment>(fixed_buf_.data(), num_fixed),
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
  std::vector<uint64_t> s1_fail_weight_;
  std::vector<uint64_t> s2_fail_weight_;

  uint32_t cur_route_stamp_ = 0;
  uint32_t cur_trial_stamp_ = 0;
  uint32_t cur_s1_visit_stamp_ = 0;
  uint32_t cur_s2_visit_stamp_ = 0;
  uint32_t cur_req_seen_stamp_ = 0;
  uint32_t cur_old_lp_stamp_ = 0;

  std::vector<uint32_t> rest_s1_stamp_;
  std::vector<uint32_t> aff_s1_stamp_;
  std::vector<uint32_t> s1_visit_buf_;
  std::vector<uint32_t> staged_s1_delta_;

  std::vector<uint32_t> rest_s2_only_stamp_;
  std::vector<uint32_t> total_rest_s2_stamp_;
  std::vector<uint32_t> s2_visit_buf_;

  std::vector<uint32_t> rest_s1_units_;
  std::vector<uint32_t> inter_s1_units_;
  std::vector<uint32_t> rest_s2_only_scenarios_;
  std::vector<uint32_t> inter_s2_scenarios_;
  std::vector<uint32_t> eval_s2_scenarios_;

  std::vector<WavelengthMask> locked_masks_;
  std::vector<BackupAssignment> fixed_buf_;
  std::vector<DisruptedRequest> disrupted_buf_;
  std::vector<uint32_t> req_seen_stamp_;
  std::vector<Lightpath> old_backup_lookup_;
  std::vector<uint32_t> old_backup_stamp_;
};

}  // namespace ton
