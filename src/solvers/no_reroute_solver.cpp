#include "ton/solvers/no_reroute_solver.hpp"

#include <algorithm>

namespace ton {

SolverStatus NoRerouteSolver::solve(const DegradedSubproblem& subproblem,
                                    SolverScratchpad& scratchpad,
                                    StagedAssignments& staged) const noexcept {
  TON_ASSERT(subproblem.path_pool != nullptr,
             "NoRerouteSolver requires non-null PathPool");
  TON_ASSERT(subproblem.disrupted.size() <= 1u,
             "NoRerouteSolver evaluates at most 1 incoming request {k}");

  const PathPool& pool = *subproblem.path_pool;
  const auto m = static_cast<ArcId>(subproblem.locked_masks.size());
  TON_BOUNDS_CHECK(m <= scratchpad.num_arcs(),
                   "NoRerouteSolver scratchpad num_arcs too small");

  std::copy_n(subproblem.locked_masks.data(), m,
              scratchpad.occupied_mask.data());

  // Verify any locked Stage-1 backup in fixed_backups (e.g., k in A_fixed(F)
  // under NoRerouteSeqRDF) avoids S_locked^(2)(F).
  for (const BackupAssignment& fb : subproblem.fixed_backups) {
    TON_ASSERT(fb.valid(), "Invalid fixed BackupAssignment in NoRerouteSolver");
    const WavelengthMask bit = wavelength_bit(fb.wavelength);
    for (const ArcId a : pool.path_arcs(fb.path_id)) {
      TON_BOUNDS_CHECK(a < m, "Fixed backup ArcId out of bounds");
      if ((scratchpad.occupied_mask[a] & bit) != 0u) {
        return SolverStatus::Infeasible;
      }
      scratchpad.occupied_mask[a] |= bit;
    }
  }

  if (subproblem.disrupted.empty()) {
    for (const BackupAssignment& fb : subproblem.fixed_backups) {
      staged.stage_append(subproblem.domain, subproblem.state_id, fb);
    }
    return SolverStatus::Feasible;
  }

  const Request& req = subproblem.disrupted[0].req;
  const PathSlice routes = pool.find(subproblem.cut_arcs, req.src, req.dst);
  if (routes.empty()) {
    return SolverStatus::Infeasible;
  }

  // Cost-ordered route scan, descending First-Fit wavelength (lambda_{W-1} ->
  // lambda_0).
  for (const PathId pid : routes) {
    WavelengthMask used = EMPTY_MASK;
    for (const ArcId a : pool.path_arcs(pid)) {
      TON_BOUNDS_CHECK(a < m, "Candidate backup ArcId out of bounds");
      used |= scratchpad.occupied_mask[a];
    }
    const WavelengthMask free_m = ~used;
    if (free_m != EMPTY_MASK) {
      const uint8_t w_star = highest_wavelength(free_m);
      for (const BackupAssignment& fb : subproblem.fixed_backups) {
        staged.stage_append(subproblem.domain, subproblem.state_id, fb);
      }
      staged.stage_append(subproblem.domain, subproblem.state_id,
                          BackupAssignment{
                              .path_id = pid,
                              .req_id = req.id,
                              .wavelength = w_star,
                          });
      return SolverStatus::Feasible;
    }
  }

  return SolverStatus::Infeasible;
}

}  // namespace ton
