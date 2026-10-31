#pragma once

#include <cmath>
#include <concepts>
#include <cstdint>
#include <span>

#include "ton/core/path_pool.hpp"
#include "ton/core/types.hpp"
#include "ton/orchestration/staging_arena.hpp"

namespace ton {

enum class SolverStatus : uint8_t {
  Feasible = 0,
  Infeasible = 1,
};

struct DisruptedRequest {
  Request req{};
  // Prior committed backup in this failure state if req is already in K_adm;
  // invalid Lightpath{} for incoming request k.
  Lightpath old_backup{};
};

struct DegradedSubproblem {
  const PathPool* path_pool = nullptr;
  CutArcs cut_arcs{};
  StateDomain domain = StateDomain::Sim;
  uint32_t state_id = INVALID_SCENARIO;
  // Forbidden slot mask per arc from surviving primaries (and immutable
  // existing backups under No-Reroute).
  std::span<const WavelengthMask> locked_masks{};
  // Locked surviving Stage-1 backups A_fixed(F) in Sequential Stage 2.
  std::span<const BackupAssignment> fixed_backups{};
  // Disrupted batch U to be routed and colored on G \ cut_arcs.
  std::span<const DisruptedRequest> disrupted{};
};

// 4-Tuple lexicographic candidate lightpath score for Proposed DSATUR
// (PHASE-3.md Section 1.4 & algorithms/DSATUR_HEURISTIC.md Section 3.3.4).
struct CandidateScore {
  static constexpr double kDamageEps = 1e-7;
  static constexpr double kCostEps = 1e-9;

  double total_damage = 0.0;   // Damage(p, lambda) + Phi_churn(r*; p, lambda)
  double path_cost = 0.0;      // c(p)
  uint8_t is_new_channel = 0;  // 1 if lambda not in Lambda_active(A), else 0
  uint8_t wavelength_rank =
      0;  // W - 1 - lambda (0 for lambda_31 -> 31 for lambda_0)

  [[nodiscard]] bool is_better_than(
      const CandidateScore& other) const noexcept {
    if (std::abs(total_damage - other.total_damage) > kDamageEps) {
      return total_damage < other.total_damage;
    }
    if (is_new_channel != other.is_new_channel) {
      return is_new_channel < other.is_new_channel;
    }
    if (std::abs(path_cost - other.path_cost) > kCostEps) {
      return path_cost < other.path_cost;
    }
    return wavelength_rank < other.wavelength_rank;
  }
};

class SolverScratchpad;

template <typename S>
concept RestorationSolver =
    requires(S solver, const DegradedSubproblem& subproblem,
             SolverScratchpad& scratchpad, StagedAssignments& staged) {
      {
        solver.solve(subproblem, scratchpad, staged)
      } noexcept -> std::same_as<SolverStatus>;
      { S::kAllowsBackupReconfiguration } -> std::convertible_to<bool>;
    };

}  // namespace ton
