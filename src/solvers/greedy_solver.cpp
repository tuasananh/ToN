#include "ton/solvers/greedy_solver.hpp"

#include <algorithm>
#include <span>

namespace ton {

SolverStatus GreedySolver::solve(const DegradedSubproblem& subproblem,
                                 SolverScratchpad& scratchpad,
                                 StagedAssignments& staged) const noexcept {
  TON_ASSERT(subproblem.path_pool != nullptr,
             "GreedySolver requires non-null PathPool");

  const PathPool& pool = *subproblem.path_pool;
  const auto m = static_cast<ArcId>(subproblem.locked_masks.size());
  TON_BOUNDS_CHECK(m <= scratchpad.num_arcs(),
                   "GreedySolver scratchpad num_arcs too small");

  std::copy_n(subproblem.locked_masks.data(), m,
              scratchpad.occupied_mask.data());

  for (const BackupAssignment& fb : subproblem.fixed_backups) {
    TON_ASSERT(fb.valid(), "Invalid fixed BackupAssignment in GreedySolver");
    const WavelengthMask bit = wavelength_bit(fb.wavelength);
    for (const ArcId a : pool.path_arcs(fb.path_id)) {
      TON_BOUNDS_CHECK(a < m, "Fixed backup ArcId out of bounds");
      if ((scratchpad.occupied_mask[a] & bit) != 0u) {
        return SolverStatus::Infeasible;
      }
      scratchpad.occupied_mask[a] |= bit;
    }
  }

  const auto num_u = static_cast<uint16_t>(subproblem.disrupted.size());
  if (num_u == 0u) {
    staged.stage_replace(subproblem.domain, subproblem.state_id,
                         subproblem.fixed_backups, {});
    return SolverStatus::Feasible;
  }
  TON_BOUNDS_CHECK(num_u <= scratchpad.max_disrupted(),
                   "GreedySolver disrupted batch exceeds scratchpad capacity");

  // Sort disrupted requests strictly in ascending order of arrival index
  // (r_1 < r_2 < ... < r_{|U|}) using zero-allocation insertion sort.
  for (uint16_t i = 0; i < num_u; ++i) {
    scratchpad.order_indices[i] = i;
  }
  for (uint16_t i = 1; i < num_u; ++i) {
    const uint16_t key_idx = scratchpad.order_indices[i];
    const RequestId key_req_id = subproblem.disrupted[key_idx].req.id;
    uint16_t j = i;
    while (j > 0u &&
           subproblem.disrupted[scratchpad.order_indices[j - 1u]].req.id >
               key_req_id) {
      scratchpad.order_indices[j] = scratchpad.order_indices[j - 1u];
      --j;
    }
    scratchpad.order_indices[j] = key_idx;
  }

  // Sequential descending First-Fit assignment (routes by cost, wavelengths
  // lambda_{W-1} -> lambda_0).
  for (uint16_t step = 0; step < num_u; ++step) {
    const uint16_t idx = scratchpad.order_indices[step];
    const Request& req = subproblem.disrupted[idx].req;
    const PathSlice routes = pool.find(subproblem.cut_arcs, req.src, req.dst);
    if (routes.empty()) {
      return SolverStatus::Infeasible;
    }

    bool assigned = false;
    for (const PathId pid : routes) {
      WavelengthMask used = EMPTY_MASK;
      for (const ArcId a : pool.path_arcs(pid)) {
        TON_BOUNDS_CHECK(a < m, "Candidate backup ArcId out of bounds");
        used |= scratchpad.occupied_mask[a];
      }
      const WavelengthMask free_m = ~used;
      if (free_m != EMPTY_MASK) {
        const uint8_t w_star = highest_wavelength(free_m);
        const WavelengthMask star_bit = wavelength_bit(w_star);
        for (const ArcId a : pool.path_arcs(pid)) {
          scratchpad.occupied_mask[a] |= star_bit;
        }
        scratchpad.solved_backups[step] = BackupAssignment{
            .path_id = pid,
            .req_id = req.id,
            .wavelength = w_star,
        };
        assigned = true;
        break;
      }
    }

    if (!assigned) {
      return SolverStatus::Infeasible;
    }
  }

  staged.stage_replace(subproblem.domain, subproblem.state_id,
                       subproblem.fixed_backups,
                       std::span<const BackupAssignment>(
                           scratchpad.solved_backups.data(), num_u));
  return SolverStatus::Feasible;
}

}  // namespace ton
