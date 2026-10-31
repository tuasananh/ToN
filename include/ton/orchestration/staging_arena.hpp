#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "ton/core/assert.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"

namespace ton {

enum class StateDomain : uint8_t {
  Sim = 0,
  SeqStage1 = 1,
  SeqStage2 = 2,
};

enum class DeltaKind : uint8_t {
  // Replaces the state's entire active backup list with the staged slice.
  ReplaceState = 0,
  // Appends a single new BackupAssignment to an immutable state (No-Reroute).
  AppendSingle = 1,
};

struct BackupAssignment {
  PathId path_id = INVALID_PATH;
  RequestId req_id = INVALID_REQUEST;
  uint8_t wavelength = INVALID_WAVELENGTH;
  uint8_t reserved = 0;

  [[nodiscard]] constexpr Lightpath lightpath() const noexcept {
    return Lightpath{.path_id = path_id, .wavelength = wavelength};
  }

  [[nodiscard]] constexpr bool valid() const noexcept {
    return req_id != INVALID_REQUEST && path_id != INVALID_PATH &&
           wavelength < NUM_WAVELENGTHS;
  }

  [[nodiscard]] constexpr bool operator==(
      const BackupAssignment&) const noexcept = default;
};

static_assert(sizeof(BackupAssignment) == 8);

struct StagedStateDelta {
  uint32_t state_id = INVALID_SCENARIO;
  uint32_t entry_offset = 0;
  uint16_t count = 0;
  StateDomain domain = StateDomain::Sim;
  DeltaKind kind = DeltaKind::ReplaceState;
};

struct StagingCheckpoint {
  uint32_t num_deltas = 0;
  uint32_t num_entries = 0;
};

// Pre-allocated staging arena supporting zero-allocation speculative trials and
// O(1) transactional rollback.
class StagedAssignments {
 public:
  StagedAssignments() = default;

  StagedAssignments(uint32_t max_deltas, uint32_t max_entries)
      : deltas_(max_deltas), entries_(max_entries) {}

  [[nodiscard]] static StagedAssignments for_regime(
      const ScenarioSet& scenarios, FailureRegime regime,
      uint16_t max_requests = 1024) {
    const uint32_t dual_cut_arcs =
        (scenarios.cut_model() == CutModel::Span) ? 4u : 2u;
    const uint32_t single_cut_arcs =
        (scenarios.cut_model() == CutModel::Span) ? 2u : 1u;

    const uint32_t dual_cap = std::min<uint32_t>(
        dual_cut_arcs * NUM_WAVELENGTHS, std::max<uint32_t>(1u, max_requests));
    const uint32_t single_cap =
        std::min<uint32_t>(single_cut_arcs * NUM_WAVELENGTHS,
                           std::max<uint32_t>(1u, max_requests));

    if (regime == FailureRegime::Simultaneous) {
      const auto max_deltas = std::max<uint32_t>(
          1u, static_cast<uint32_t>(scenarios.sim_scenarios().size()));
      const uint32_t max_entries =
          std::max<uint32_t>(64u, max_deltas * dual_cap);
      return StagedAssignments(max_deltas, max_entries);
    }

    const uint32_t num_s1 = scenarios.num_units();
    const auto num_s2 = static_cast<uint32_t>(scenarios.seq_scenarios().size());
    const uint32_t max_deltas = std::max<uint32_t>(1u, num_s1 + num_s2);
    const uint32_t max_entries =
        std::max<uint32_t>(64u, num_s1 * single_cap + num_s2 * dual_cap);
    return StagedAssignments(max_deltas, max_entries);
  }

  // O(1) rollback: resets counters without touching committed NetworkState.
  void rollback() noexcept {
    num_deltas_ = 0;
    num_entries_ = 0;
  }

  [[nodiscard]] StagingCheckpoint checkpoint() const noexcept {
    return StagingCheckpoint{
        .num_deltas = num_deltas_,
        .num_entries = num_entries_,
    };
  }

  void rollback_to(StagingCheckpoint cp) noexcept {
    TON_BOUNDS_CHECK(
        cp.num_deltas <= num_deltas_ && cp.num_entries <= num_entries_,
        "StagedAssignments::rollback_to invalid checkpoint");
    num_deltas_ = cp.num_deltas;
    num_entries_ = cp.num_entries;
  }

  uint32_t stage_replace(
      StateDomain domain, uint32_t state_id,
      std::span<const BackupAssignment> fixed_backups,
      std::span<const BackupAssignment> newly_solved) noexcept {
    const size_t total = fixed_backups.size() + newly_solved.size();
    TON_BOUNDS_CHECK(num_deltas_ < deltas_.size(),
                     "StagedAssignments delta capacity exceeded");
    TON_BOUNDS_CHECK(
        static_cast<size_t>(num_entries_) + total <= entries_.size(),
        "StagedAssignments entry capacity exceeded");

    const uint32_t offset = num_entries_;
    for (const BackupAssignment& ba : fixed_backups) {
      TON_ASSERT(ba.valid(), "Staging invalid fixed BackupAssignment");
      entries_[num_entries_++] = ba;
    }
    for (const BackupAssignment& ba : newly_solved) {
      TON_ASSERT(ba.valid(), "Staging invalid solved BackupAssignment");
      entries_[num_entries_++] = ba;
    }

    const uint32_t delta_idx = num_deltas_++;
    deltas_[delta_idx] = StagedStateDelta{
        .state_id = state_id,
        .entry_offset = offset,
        .count = static_cast<uint16_t>(total),
        .domain = domain,
        .kind = DeltaKind::ReplaceState,
    };
    return delta_idx;
  }

  uint32_t stage_replace(
      StateDomain domain, uint32_t state_id,
      std::span<const BackupAssignment> all_backups) noexcept {
    return stage_replace(domain, state_id, {}, all_backups);
  }

  uint32_t stage_append(StateDomain domain, uint32_t state_id,
                        BackupAssignment single) noexcept {
    TON_ASSERT(single.valid(), "Staging invalid single BackupAssignment");
    TON_BOUNDS_CHECK(num_deltas_ < deltas_.size(),
                     "StagedAssignments delta capacity exceeded");
    TON_BOUNDS_CHECK(num_entries_ < entries_.size(),
                     "StagedAssignments entry capacity exceeded");

    const uint32_t offset = num_entries_++;
    entries_[offset] = single;

    const uint32_t delta_idx = num_deltas_++;
    deltas_[delta_idx] = StagedStateDelta{
        .state_id = state_id,
        .entry_offset = offset,
        .count = 1,
        .domain = domain,
        .kind = DeltaKind::AppendSingle,
    };
    return delta_idx;
  }

  [[nodiscard]] uint32_t num_deltas() const noexcept { return num_deltas_; }
  [[nodiscard]] uint32_t num_entries() const noexcept { return num_entries_; }
  [[nodiscard]] bool empty() const noexcept { return num_deltas_ == 0; }

  [[nodiscard]] std::span<const StagedStateDelta> deltas() const noexcept {
    return std::span<const StagedStateDelta>(deltas_.data(), num_deltas_);
  }

  [[nodiscard]] const StagedStateDelta& delta(
      uint32_t delta_idx) const noexcept {
    TON_BOUNDS_CHECK(delta_idx < num_deltas_,
                     "StagedAssignments::delta index out of bounds");
    return deltas_[delta_idx];
  }

  [[nodiscard]] std::span<const BackupAssignment> delta_entries(
      const StagedStateDelta& d) const noexcept {
    TON_BOUNDS_CHECK(
        static_cast<size_t>(d.entry_offset) + d.count <= num_entries_,
        "StagedAssignments::delta_entries slice out of bounds");
    return std::span<const BackupAssignment>(entries_.data() + d.entry_offset,
                                             d.count);
  }

  [[nodiscard]] std::span<const BackupAssignment> delta_entries(
      uint32_t delta_idx) const noexcept {
    return delta_entries(delta(delta_idx));
  }

 private:
  std::vector<StagedStateDelta> deltas_;
  std::vector<BackupAssignment> entries_;
  uint32_t num_deltas_ = 0;
  uint32_t num_entries_ = 0;
};

}  // namespace ton
