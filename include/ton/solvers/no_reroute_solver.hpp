#pragma once

#include "ton/orchestration/staging_arena.hpp"
#include "ton/solvers/scratchpad.hpp"
#include "ton/solvers/solver_interface.hpp"

namespace ton {

// Comparative Baseline 2: Immutable Backup Provisioning
// (NoRerouteSimRDF & NoRerouteSeqRDF, algorithms/NO_REROUTE_BASELINE.md).
class NoRerouteSolver {
 public:
  static constexpr bool kAllowsBackupReconfiguration = false;

  [[nodiscard]] SolverStatus solve(const DegradedSubproblem& subproblem,
                                   SolverScratchpad& scratchpad,
                                   StagedAssignments& staged) const noexcept;
};

static_assert(RestorationSolver<NoRerouteSolver>);

}  // namespace ton
