#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ton/core/assert.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/types.hpp"
#include "ton/orchestration/staging_arena.hpp"

namespace ton {

// Reusable per-worker scratchpad pre-allocated before entering the online
// request provisioning loop (t = 0..999). Guarantees zero heap allocations
// inside all inner solver invocations.
class SolverScratchpad {
 public:
  SolverScratchpad() = default;

  SolverScratchpad(ArcId num_arcs, uint16_t max_disrupted = 256,
                   uint16_t max_k = 16) {
    init(num_arcs, max_disrupted, max_k);
  }

  void init(ArcId num_arcs, uint16_t max_disrupted = 256, uint16_t max_k = 16) {
    num_arcs_ = num_arcs;
    max_disrupted_ = std::max<uint16_t>(1u, max_disrupted);
    max_k_ = std::max<uint16_t>(1u, max_k);

    occupied_mask.assign(num_arcs_, EMPTY_MASK);
    free_mask.assign(static_cast<size_t>(max_disrupted_) * max_k_, EMPTY_MASK);
    cand_slice.assign(max_disrupted_, PathSlice{});
    union_free_mask.assign(max_disrupted_, EMPTY_MASK);
    omega_lambda.assign(max_disrupted_, uint8_t{0});
    omega_pairs.assign(max_disrupted_, uint16_t{0});
    uncolored.assign(max_disrupted_, uint8_t{0});
    order_indices.assign(max_disrupted_, uint16_t{0});
    crit_neighbors.assign(max_disrupted_, uint16_t{0});
    solved_backups.assign(max_disrupted_, BackupAssignment{});
    psi.assign(static_cast<size_t>(num_arcs_) * NUM_WAVELENGTHS, 0.0);
    arc_stamp.assign(num_arcs_, uint32_t{0});
    cur_arc_stamp_ = 0;
  }

  [[nodiscard]] ArcId num_arcs() const noexcept { return num_arcs_; }
  [[nodiscard]] uint16_t max_disrupted() const noexcept {
    return max_disrupted_;
  }
  [[nodiscard]] uint16_t max_k() const noexcept { return max_k_; }

  [[nodiscard]] WavelengthMask& route_free_mask(uint16_t u_idx,
                                                uint16_t c_idx) noexcept {
    TON_BOUNDS_CHECK(u_idx < max_disrupted_ && c_idx < max_k_,
                     "SolverScratchpad::route_free_mask out of bounds");
    return free_mask[static_cast<size_t>(u_idx) * max_k_ + c_idx];
  }

  [[nodiscard]] WavelengthMask route_free_mask(uint16_t u_idx,
                                               uint16_t c_idx) const noexcept {
    TON_BOUNDS_CHECK(u_idx < max_disrupted_ && c_idx < max_k_,
                     "SolverScratchpad::route_free_mask out of bounds");
    return free_mask[static_cast<size_t>(u_idx) * max_k_ + c_idx];
  }

  [[nodiscard]] uint32_t next_arc_stamp() noexcept {
    ++cur_arc_stamp_;
    if (cur_arc_stamp_ == 0u) [[unlikely]] {
      std::fill(arc_stamp.begin(), arc_stamp.end(), uint32_t{0});
      cur_arc_stamp_ = 1u;
    }
    return cur_arc_stamp_;
  }

  void reset_counters() noexcept {
    theorem1_bypass_steps = 0;
    theorem1_fc_checked_steps = 0;
  }

  std::vector<WavelengthMask> occupied_mask;
  std::vector<WavelengthMask> free_mask;
  std::vector<PathSlice> cand_slice;
  std::vector<WavelengthMask> union_free_mask;
  std::vector<uint8_t> omega_lambda;
  std::vector<uint16_t> omega_pairs;
  std::vector<uint8_t> uncolored;
  std::vector<uint16_t> order_indices;
  std::vector<uint16_t> crit_neighbors;
  std::vector<BackupAssignment> solved_backups;
  std::vector<double> psi;
  std::vector<uint32_t> arc_stamp;

  // Diagnostic telemetry counters for Theorem 1 verification.
  uint64_t theorem1_bypass_steps = 0;
  uint64_t theorem1_fc_checked_steps = 0;

 private:
  ArcId num_arcs_ = 0;
  uint16_t max_disrupted_ = 0;
  uint16_t max_k_ = 0;
  uint32_t cur_arc_stamp_ = 0;
};

}  // namespace ton
