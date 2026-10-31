#pragma once

#include "ton/orchestration/staging_arena.hpp"
#include "ton/solvers/scratchpad.hpp"
#include "ton/solvers/solver_interface.hpp"

namespace ton {

// Comparative Baseline 1: Static Arrival-Order Greedy First-Fit
// (GreedySimRDF & GreedySeqRDF, algorithms/GREEDY_BASELINE.md).
class GreedySolver {
 public:
  static constexpr bool kAllowsBackupReconfiguration = true;

  [[nodiscard]] SolverStatus solve(const DegradedSubproblem& subproblem,
                                   SolverScratchpad& scratchpad,
                                   StagedAssignments& staged) const noexcept;
};

static_assert(RestorationSolver<GreedySolver>);

}  // namespace ton
