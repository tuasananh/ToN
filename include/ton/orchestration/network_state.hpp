#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ton/core/assert.hpp"
#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"
#include "ton/orchestration/staging_arena.hpp"

namespace ton {

// Cache-resident network state tracking immutable primary allocations,
// L3-resident per-scenario backup bitmasks, compact per-state backup
// assignments, and L1-resident backup_slot_refcount[M][W].
class NetworkState {
 public:
  NetworkState(const Graph& graph, const ScenarioSet& scenarios,
               FailureRegime regime, uint16_t max_requests = 1024);

  // Resets all working and backup allocations to empty with zero heap
  // allocations.
  void reset() noexcept;

  [[nodiscard]] const Graph& graph() const noexcept { return *graph_; }
  [[nodiscard]] const ScenarioSet& scenarios() const noexcept {
    return *scenarios_;
  }
  [[nodiscard]] FailureRegime regime() const noexcept { return regime_; }
  [[nodiscard]] ArcId num_arcs() const noexcept { return num_arcs_; }
  [[nodiscard]] uint16_t max_requests() const noexcept { return max_requests_; }

  [[nodiscard]] uint16_t num_admitted() const noexcept { return num_admitted_; }
  [[nodiscard]] uint32_t num_primary_slots() const noexcept {
    return num_primary_slots_;
  }

  [[nodiscard]] std::span<const RequestId> admitted_ids() const noexcept {
    return std::span<const RequestId>(admitted_ids_.data(), num_admitted_);
  }

  [[nodiscard]] bool is_admitted(RequestId r) const noexcept {
    return r < max_requests_ && is_admitted_[r] != 0u;
  }

  [[nodiscard]] const Request& request(RequestId r) const noexcept {
    TON_BOUNDS_CHECK(is_admitted(r),
                     "NetworkState::request RequestId not admitted");
    return requests_[r];
  }

  [[nodiscard]] Lightpath primary_lightpath(RequestId r) const noexcept {
    TON_BOUNDS_CHECK(is_admitted(r),
                     "NetworkState::primary_lightpath RequestId not admitted");
    return primary_lightpaths_[r];
  }

  // L1-resident primary occupancy bitmask per arc (M * 4 bytes <= 704 B).
  [[nodiscard]] WavelengthMask primary_mask(ArcId a) const noexcept {
    TON_BOUNDS_CHECK(a < num_arcs_,
                     "NetworkState::primary_mask ArcId out of bounds");
    return primary_mask_[a];
  }

  [[nodiscard]] std::span<const WavelengthMask> primary_masks() const noexcept {
    return primary_mask_;
  }

  // L1-resident primary owner table (M * 32 * 2 bytes <= 11.3 KB).
  [[nodiscard]] RequestId primary_owner(ArcId a, uint8_t w) const noexcept {
    TON_BOUNDS_CHECK(a < num_arcs_ && w < NUM_WAVELENGTHS,
                     "NetworkState::primary_owner index out of bounds");
    return primary_owner_[static_cast<size_t>(a) * NUM_WAVELENGTHS + w];
  }

  // L1-resident backup slot reference count across all active failure states
  // (M * 32 * 2 bytes <= 11.3 KB).
  [[nodiscard]] uint16_t backup_slot_refcount(ArcId a,
                                              uint8_t w) const noexcept {
    TON_BOUNDS_CHECK(a < num_arcs_ && w < NUM_WAVELENGTHS,
                     "NetworkState::backup_slot_refcount index out of bounds");
    return backup_slot_refcount_[static_cast<size_t>(a) * NUM_WAVELENGTHS + w];
  }

  [[nodiscard]] std::span<const uint16_t> backup_slot_refcounts()
      const noexcept {
    return backup_slot_refcount_;
  }

  // L1-resident bitmask per arc caching (backup_slot_refcount[a][w] > 0)
  // (M * 4 bytes <= 704 B) for sub-50ns SIMD/popcount spectral telemetry.
  [[nodiscard]] WavelengthMask backup_active_mask(ArcId a) const noexcept {
    TON_BOUNDS_CHECK(a < num_arcs_,
                     "NetworkState::backup_active_mask ArcId out of bounds");
    return backup_active_mask_[a];
  }

  [[nodiscard]] std::span<const WavelengthMask> backup_active_masks()
      const noexcept {
    return backup_active_mask_;
  }

  // Fast bitwise primary route availability mask (~union of primary_mask[a]).
  [[nodiscard]] WavelengthMask free_primary_wavelengths(
      std::span<const ArcId> path_arcs) const noexcept {
    WavelengthMask occupied = EMPTY_MASK;
    for (const ArcId a : path_arcs) {
      TON_BOUNDS_CHECK(a < num_arcs_, "path_arcs ArcId out of bounds");
      occupied |= primary_mask_[a];
    }
    return ~occupied;
  }

  [[nodiscard]] bool is_primary_clash_free(std::span<const ArcId> path_arcs,
                                           uint8_t w) const noexcept {
    TON_BOUNDS_CHECK(w < NUM_WAVELENGTHS, "wavelength out of bounds");
    const WavelengthMask bit = wavelength_bit(w);
    for (const ArcId a : path_arcs) {
      TON_BOUNDS_CHECK(a < num_arcs_, "path_arcs ArcId out of bounds");
      if ((primary_mask_[a] & bit) != 0u) {
        return false;
      }
    }
    return true;
  }

  // Simultaneous scenario backup masks & active assignment lists.
  [[nodiscard]] WavelengthMask sim_backup_mask(ScenarioId f,
                                               ArcId a) const noexcept {
    TON_BOUNDS_CHECK(regime_ == FailureRegime::Simultaneous,
                     "sim_backup_mask requires Simultaneous regime");
    return sim_table_.mask(f, a, num_arcs_);
  }

  [[nodiscard]] std::span<const BackupAssignment> sim_backups(
      ScenarioId f) const noexcept {
    TON_BOUNDS_CHECK(regime_ == FailureRegime::Simultaneous,
                     "sim_backups requires Simultaneous regime");
    return sim_table_.backups(f);
  }

  [[nodiscard]] Lightpath find_sim_backup(ScenarioId f,
                                          RequestId r) const noexcept {
    return find_in_list(sim_backups(f), r);
  }

  // Sequential Stage-1 unit backup masks & active assignment lists.
  [[nodiscard]] WavelengthMask seq_stage1_backup_mask(FailureUnitId x1,
                                                      ArcId a) const noexcept {
    TON_BOUNDS_CHECK(regime_ == FailureRegime::Sequential,
                     "seq_stage1_backup_mask requires Sequential regime");
    return s1_table_.mask(x1, a, num_arcs_);
  }

  [[nodiscard]] std::span<const BackupAssignment> seq_stage1_backups(
      FailureUnitId x1) const noexcept {
    TON_BOUNDS_CHECK(regime_ == FailureRegime::Sequential,
                     "seq_stage1_backups requires Sequential regime");
    return s1_table_.backups(x1);
  }

  [[nodiscard]] Lightpath find_seq_stage1_backup(FailureUnitId x1,
                                                 RequestId r) const noexcept {
    return find_in_list(seq_stage1_backups(x1), r);
  }

  // Sequential Stage-2 scenario backup masks & active assignment lists.
  [[nodiscard]] WavelengthMask seq_stage2_backup_mask(ScenarioId f,
                                                      ArcId a) const noexcept {
    TON_BOUNDS_CHECK(regime_ == FailureRegime::Sequential,
                     "seq_stage2_backup_mask requires Sequential regime");
    return s2_table_.mask(f, a, num_arcs_);
  }

  [[nodiscard]] std::span<const BackupAssignment> seq_stage2_backups(
      ScenarioId f) const noexcept {
    TON_BOUNDS_CHECK(regime_ == FailureRegime::Sequential,
                     "seq_stage2_backups requires Sequential regime");
    return s2_table_.backups(f);
  }

  [[nodiscard]] Lightpath find_seq_stage2_backup(ScenarioId f,
                                                 RequestId r) const noexcept {
    return find_in_list(seq_stage2_backups(f), r);
  }

  [[nodiscard]] uint32_t num_domain_states(StateDomain domain) const noexcept {
    return table_for(domain).num_states;
  }

  [[nodiscard]] WavelengthMask state_backup_mask(StateDomain domain,
                                                 uint32_t state_id,
                                                 ArcId a) const noexcept {
    return table_for(domain).mask(state_id, a, num_arcs_);
  }

  [[nodiscard]] std::span<const BackupAssignment> state_backups(
      StateDomain domain, uint32_t state_id) const noexcept {
    return table_for(domain).backups(state_id);
  }

  // Zero-allocation collection of failure states in `domain` (excluding those
  // where `excluded_stamp[id] == excluded_val`) that currently reserve
  // `(a, wavelength)` for at least one `a` in `primary_arcs`.
  uint32_t collect_interfering_states(
      StateDomain domain, std::span<const ArcId> primary_arcs,
      uint8_t wavelength, std::span<const uint32_t> excluded_stamp,
      uint32_t excluded_val, std::span<uint32_t> visit_stamp,
      uint32_t visit_val, std::span<uint32_t> out_state_ids) const noexcept;

  // Fast check whether ANY non-excluded state in `domain` has a backup
  // colliding with `(primary_arcs, wavelength)` (used by No-Reroute gate).
  [[nodiscard]] bool has_interfering_state(
      StateDomain domain, std::span<const ArcId> primary_arcs,
      uint8_t wavelength, std::span<const uint32_t> excluded_stamp,
      uint32_t excluded_val) const noexcept;

  // Atomically commits an admitted primary lightpath and all staged backup
  // deltas, incrementally updating backup_slot_refcount in O(Delta) with zero
  // heap allocations.
  void commit(const Request& req, Lightpath primary_lp,
              const StagedAssignments& staged,
              const PathPool& path_pool) noexcept;

 private:
  struct StateBackupTable {
    uint32_t num_states = 0;
    uint32_t num_blocks = 0;
    uint16_t per_state_cap = 0;
    std::vector<WavelengthMask> masks;
    std::vector<WavelengthMask> block_union_masks;
    std::vector<uint16_t> counts;
    std::vector<BackupAssignment> assignments;

    void init(uint32_t states, ArcId num_arcs, uint16_t cap);
    void clear() noexcept;

    [[nodiscard]] WavelengthMask mask(uint32_t state_id, ArcId a,
                                      ArcId num_arcs) const noexcept {
      TON_BOUNDS_CHECK(state_id < num_states && a < num_arcs,
                       "StateBackupTable::mask index out of bounds");
      return masks[static_cast<size_t>(state_id) * num_arcs + a];
    }

    [[nodiscard]] std::span<const BackupAssignment> backups(
        uint32_t state_id) const noexcept {
      TON_BOUNDS_CHECK(state_id < num_states,
                       "StateBackupTable::backups state_id out of bounds");
      return std::span<const BackupAssignment>(
          assignments.data() + static_cast<size_t>(state_id) * per_state_cap,
          counts[state_id]);
    }
  };

  [[nodiscard]] static Lightpath find_in_list(
      std::span<const BackupAssignment> list, RequestId r) noexcept {
    for (const BackupAssignment& ba : list) {
      if (ba.req_id == r) {
        return ba.lightpath();
      }
    }
    return Lightpath{};
  }

  [[nodiscard]] const StateBackupTable& table_for(
      StateDomain domain) const noexcept;
  [[nodiscard]] StateBackupTable& table_for(StateDomain domain) noexcept;

  void refresh_block_union(StateBackupTable& tbl, uint32_t blk,
                           ArcId a) noexcept;

  const Graph* graph_ = nullptr;
  const ScenarioSet* scenarios_ = nullptr;
  FailureRegime regime_ = FailureRegime::Simultaneous;
  ArcId num_arcs_ = 0;
  uint16_t max_requests_ = 0;

  uint16_t num_admitted_ = 0;
  uint32_t num_primary_slots_ = 0;

  std::vector<WavelengthMask> primary_mask_;
  std::vector<WavelengthMask> backup_active_mask_;
  std::vector<RequestId> primary_owner_;
  std::vector<uint16_t> backup_slot_refcount_;

  std::vector<uint8_t> is_admitted_;
  std::vector<Request> requests_;
  std::vector<Lightpath> primary_lightpaths_;
  std::vector<RequestId> admitted_ids_;

  StateBackupTable sim_table_;
  StateBackupTable s1_table_;
  StateBackupTable s2_table_;

  std::vector<uint64_t> dirty_arc_words_;
};

}  // namespace ton
