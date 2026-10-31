#pragma once

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "catch_amalgamated.hpp"
#include "ton/core/assert.hpp"
#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"
#include "ton/orchestration/network_state.hpp"
#include "ton/orchestration/provisioning_harness.hpp"
#include "ton/orchestration/telemetry.hpp"

namespace ton::test {

[[nodiscard]] inline std::filesystem::path repo_root() {
#if defined(TON_REPO_ROOT)
  return std::filesystem::path(TON_REPO_ROOT);
#else
  return std::filesystem::current_path();
#endif
}

// Independent full-state verifiers of the 5 mathematical invariants (PROBLEM.md
// Section 9).
void audit_simultaneous_state_invariants(const NetworkState& state,
                                         const PathPool& pool);

void audit_sequential_state_invariants(const NetworkState& state,
                                       const PathPool& pool);

// Dynamic heap allocation monitoring for Phase 3 zero-allocation online loops.
void arm_zero_alloc_monitor() noexcept;
void disarm_zero_alloc_monitor() noexcept;
[[nodiscard]] uint64_t tracked_allocations() noexcept;
[[nodiscard]] bool is_asan_active() noexcept;

}  // namespace ton::test

