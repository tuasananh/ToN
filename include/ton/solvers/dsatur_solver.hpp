#pragma once

#include "ton/orchestration/staging_arena.hpp"
#include "ton/solvers/scratchpad.hpp"
#include "ton/solvers/solver_interface.hpp"

namespace ton {

// Proposed Heuristic: Contention- & Stability-Aware DSATUR Coloring
// (SimRDF & SeqRDF, algorithms/DSATUR_HEURISTIC.md).
class DsaturSolver {
 public:
  static constexpr bool kAllowsBackupReconfiguration = true;
  static constexpr double kBetaInertia =
      1.0 / static_cast<double>(NUM_WAVELENGTHS);

  static_assert(kBetaInertia == 0.03125);

  [[nodiscard]] SolverStatus solve(const DegradedSubproblem& subproblem,
                                   SolverScratchpad& scratchpad,
                                   StagedAssignments& staged) const noexcept;
};

static_assert(RestorationSolver<DsaturSolver>);

}  // namespace ton
