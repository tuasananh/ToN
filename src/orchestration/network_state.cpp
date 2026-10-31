#include "ton/orchestration/network_state.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace ton {

void NetworkState::StateBackupTable::init(uint32_t states, ArcId num_arcs,
                                          uint16_t cap) {
  num_states = states;
  num_blocks = (states + 63u) / 64u;
  per_state_cap = cap;
  masks.assign(static_cast<size_t>(states) * num_arcs, EMPTY_MASK);
  block_union_masks.assign(static_cast<size_t>(num_arcs) * num_blocks,
                           EMPTY_MASK);
  counts.assign(states, 0u);
  assignments.assign(static_cast<size_t>(states) * cap, BackupAssignment{});
}

void NetworkState::StateBackupTable::clear() noexcept {
  std::fill(masks.begin(), masks.end(), EMPTY_MASK);
  std::fill(block_union_masks.begin(), block_union_masks.end(), EMPTY_MASK);
  std::fill(counts.begin(), counts.end(), uint16_t{0});
}

NetworkState::NetworkState(const Graph& graph, const ScenarioSet& scenarios,
                           FailureRegime regime, uint16_t max_requests)
    : graph_(&graph),
      scenarios_(&scenarios),
      regime_(regime),
      num_arcs_(graph.num_arcs()),
      max_requests_(std::max<uint16_t>(1u, max_requests)),
      primary_mask_(num_arcs_, EMPTY_MASK),
      backup_active_mask_(num_arcs_, EMPTY_MASK),
      primary_owner_(static_cast<size_t>(num_arcs_) * NUM_WAVELENGTHS,
                     INVALID_REQUEST),
      backup_slot_refcount_(static_cast<size_t>(num_arcs_) * NUM_WAVELENGTHS,
                            uint16_t{0}),
      is_admitted_(max_requests_, uint8_t{0}),
      requests_(max_requests_, Request{}),
      primary_lightpaths_(max_requests_, Lightpath{}),
      admitted_ids_(max_requests_, INVALID_REQUEST) {
  const uint16_t dual_cut_arcs =
      (scenarios.cut_model() == CutModel::Span) ? 4u : 2u;
  const uint16_t single_cut_arcs =
      (scenarios.cut_model() == CutModel::Span) ? 2u : 1u;

  const auto dual_cap = static_cast<uint16_t>(std::min<uint32_t>(
      static_cast<uint32_t>(dual_cut_arcs) * NUM_WAVELENGTHS, max_requests_));
  const auto single_cap = static_cast<uint16_t>(std::min<uint32_t>(
      static_cast<uint32_t>(single_cut_arcs) * NUM_WAVELENGTHS, max_requests_));

  if (regime_ == FailureRegime::Simultaneous) {
    sim_table_.init(static_cast<uint32_t>(scenarios.sim_scenarios().size()),
                    num_arcs_, dual_cap);
  } else {
    s1_table_.init(scenarios.num_units(), num_arcs_, single_cap);
    s2_table_.init(static_cast<uint32_t>(scenarios.seq_scenarios().size()),
                   num_arcs_, dual_cap);
  }
}

void NetworkState::reset() noexcept {
  num_admitted_ = 0;
  num_primary_slots_ = 0;
  std::fill(primary_mask_.begin(), primary_mask_.end(), EMPTY_MASK);
  std::fill(backup_active_mask_.begin(), backup_active_mask_.end(), EMPTY_MASK);
  std::fill(primary_owner_.begin(), primary_owner_.end(), INVALID_REQUEST);
  std::fill(backup_slot_refcount_.begin(), backup_slot_refcount_.end(),
            uint16_t{0});
  std::fill(is_admitted_.begin(), is_admitted_.end(), uint8_t{0});
  sim_table_.clear();
  s1_table_.clear();
  s2_table_.clear();
}

const NetworkState::StateBackupTable& NetworkState::table_for(
    StateDomain domain) const noexcept {
  switch (domain) {
    case StateDomain::Sim:
      TON_ASSERT(regime_ == FailureRegime::Simultaneous,
                 "StateDomain::Sim accessed under Sequential regime");
      return sim_table_;
    case StateDomain::SeqStage1:
      TON_ASSERT(regime_ == FailureRegime::Sequential,
                 "StateDomain::SeqStage1 accessed under Simultaneous regime");
      return s1_table_;
    case StateDomain::SeqStage2:
      TON_ASSERT(regime_ == FailureRegime::Sequential,
                 "StateDomain::SeqStage2 accessed under Simultaneous regime");
      return s2_table_;
  }
  return sim_table_;
}

NetworkState::StateBackupTable& NetworkState::table_for(
    StateDomain domain) noexcept {
  switch (domain) {
    case StateDomain::Sim:
      TON_ASSERT(regime_ == FailureRegime::Simultaneous,
                 "StateDomain::Sim accessed under Sequential regime");
      return sim_table_;
    case StateDomain::SeqStage1:
      TON_ASSERT(regime_ == FailureRegime::Sequential,
                 "StateDomain::SeqStage1 accessed under Simultaneous regime");
      return s1_table_;
    case StateDomain::SeqStage2:
      TON_ASSERT(regime_ == FailureRegime::Sequential,
                 "StateDomain::SeqStage2 accessed under Simultaneous regime");
      return s2_table_;
  }
  return sim_table_;
}

void NetworkState::refresh_block_union(StateBackupTable& tbl, uint32_t blk,
                                       ArcId a) noexcept {
  const uint32_t begin = blk * 64u;
  const uint32_t end = std::min<uint32_t>(begin + 64u, tbl.num_states);
  WavelengthMask acc = EMPTY_MASK;
  for (uint32_t f = begin; f < end; ++f) {
    acc |= tbl.masks[static_cast<size_t>(f) * num_arcs_ + a];
  }
  tbl.block_union_masks[static_cast<size_t>(a) * tbl.num_blocks + blk] = acc;
}

uint32_t NetworkState::collect_interfering_states(
    StateDomain domain, std::span<const ArcId> primary_arcs, uint8_t wavelength,
    std::span<const uint32_t> excluded_stamp, uint32_t excluded_val,
    std::span<uint32_t> visit_stamp, uint32_t visit_val,
    std::span<uint32_t> out_state_ids) const noexcept {
  TON_BOUNDS_CHECK(wavelength < NUM_WAVELENGTHS,
                   "collect_interfering_states wavelength out of bounds");
  const StateBackupTable& tbl = table_for(domain);
  if (tbl.num_states == 0u) {
    return 0u;
  }
  TON_BOUNDS_CHECK(visit_stamp.size() >= tbl.num_states,
                   "visit_stamp buffer too small");
  TON_BOUNDS_CHECK(
      excluded_stamp.empty() || excluded_stamp.size() >= tbl.num_states,
      "excluded_stamp buffer too small");

  const WavelengthMask w_bit = wavelength_bit(wavelength);
  uint32_t out_count = 0;

  for (const ArcId a : primary_arcs) {
    TON_BOUNDS_CHECK(a < num_arcs_, "primary_arcs ArcId out of bounds");
    if (backup_slot_refcount_[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                              wavelength] == 0u) {
      continue;
    }

    const WavelengthMask* blk_row =
        tbl.block_union_masks.data() + static_cast<size_t>(a) * tbl.num_blocks;
    for (uint32_t blk = 0; blk < tbl.num_blocks; ++blk) {
      if ((blk_row[blk] & w_bit) == 0u) {
        continue;
      }
      const uint32_t begin = blk * 64u;
      const uint32_t end = std::min<uint32_t>(begin + 64u, tbl.num_states);
      for (uint32_t f = begin; f < end; ++f) {
        if (!excluded_stamp.empty() && excluded_stamp[f] == excluded_val) {
          continue;
        }
        if (visit_stamp[f] == visit_val) {
          continue;
        }
        if ((tbl.masks[static_cast<size_t>(f) * num_arcs_ + a] & w_bit) != 0u) {
          visit_stamp[f] = visit_val;
          TON_BOUNDS_CHECK(out_count < out_state_ids.size(),
                           "out_state_ids capacity exceeded");
          out_state_ids[out_count++] = f;
        }
      }
    }
  }
  return out_count;
}

bool NetworkState::has_interfering_state(
    StateDomain domain, std::span<const ArcId> primary_arcs, uint8_t wavelength,
    std::span<const uint32_t> excluded_stamp,
    uint32_t excluded_val) const noexcept {
  TON_BOUNDS_CHECK(wavelength < NUM_WAVELENGTHS,
                   "has_interfering_state wavelength out of bounds");
  const StateBackupTable& tbl = table_for(domain);
  if (tbl.num_states == 0u) {
    return false;
  }
  TON_BOUNDS_CHECK(
      excluded_stamp.empty() || excluded_stamp.size() >= tbl.num_states,
      "excluded_stamp buffer too small");

  const WavelengthMask w_bit = wavelength_bit(wavelength);
  for (const ArcId a : primary_arcs) {
    TON_BOUNDS_CHECK(a < num_arcs_, "primary_arcs ArcId out of bounds");
    if (backup_slot_refcount_[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                              wavelength] == 0u) {
      continue;
    }

    const WavelengthMask* blk_row =
        tbl.block_union_masks.data() + static_cast<size_t>(a) * tbl.num_blocks;
    for (uint32_t blk = 0; blk < tbl.num_blocks; ++blk) {
      if ((blk_row[blk] & w_bit) == 0u) {
        continue;
      }
      const uint32_t begin = blk * 64u;
      const uint32_t end = std::min<uint32_t>(begin + 64u, tbl.num_states);
      for (uint32_t f = begin; f < end; ++f) {
        if (!excluded_stamp.empty() && excluded_stamp[f] == excluded_val) {
          continue;
        }
        if ((tbl.masks[static_cast<size_t>(f) * num_arcs_ + a] & w_bit) != 0u) {
          return true;
        }
      }
    }
  }
  return false;
}

void NetworkState::commit(const Request& req, Lightpath primary_lp,
                          const StagedAssignments& staged,
                          const PathPool& path_pool) noexcept {
  TON_BOUNDS_CHECK(req.id < max_requests_,
                   "NetworkState::commit RequestId exceeds max_requests");
  TON_ASSERT(!is_admitted(req.id),
             "NetworkState::commit duplicate admission of RequestId");
  TON_ASSERT(primary_lp.valid(),
             "NetworkState::commit invalid primary Lightpath");

  const WavelengthMask prim_bit = wavelength_bit(primary_lp.wavelength);
  for (const ArcId a : path_pool.path_arcs(primary_lp.path_id)) {
    TON_BOUNDS_CHECK(a < num_arcs_,
                     "NetworkState::commit primary ArcId out of bounds");
    TON_ASSERT((primary_mask_[a] & prim_bit) == 0u,
               "INV-1 violation: primary wavelength clash on commit");
    TON_ASSERT(primary_owner_[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                              primary_lp.wavelength] == INVALID_REQUEST,
               "INV-1 violation: primary owner already occupied");
    primary_mask_[a] |= prim_bit;
    primary_owner_[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                   primary_lp.wavelength] = req.id;
    ++num_primary_slots_;
  }

  is_admitted_[req.id] = 1u;
  requests_[req.id] = req;
  primary_lightpaths_[req.id] = primary_lp;
  admitted_ids_[num_admitted_++] = req.id;

  constexpr size_t kFastDirtyArcLimit = 512;

  for (const StagedStateDelta& d : staged.deltas()) {
    StateBackupTable& tbl = table_for(d.domain);
    const uint32_t f = d.state_id;
    TON_BOUNDS_CHECK(f < tbl.num_states,
                     "NetworkState::commit state_id out of bounds");
    const uint32_t blk = f >> 6;

    if (d.kind == DeltaKind::AppendSingle) {
      const auto entries = staged.delta_entries(d);
      TON_ASSERT(entries.size() == 1u,
                 "AppendSingle delta must contain 1 entry");
      const BackupAssignment ba = entries[0];
      TON_ASSERT(ba.valid(), "Invalid BackupAssignment in AppendSingle");
      TON_BOUNDS_CHECK(tbl.counts[f] < tbl.per_state_cap,
                       "State backup capacity exceeded on AppendSingle");

      const WavelengthMask bit = wavelength_bit(ba.wavelength);
      for (const ArcId a : path_pool.path_arcs(ba.path_id)) {
        TON_BOUNDS_CHECK(a < num_arcs_, "Backup ArcId out of bounds");
        WavelengthMask& sm = tbl.masks[static_cast<size_t>(f) * num_arcs_ + a];
        TON_ASSERT((sm & bit) == 0u, "Backup wavelength clash on AppendSingle");
        sm |= bit;
        tbl.block_union_masks[static_cast<size_t>(a) * tbl.num_blocks + blk] |=
            bit;
        uint16_t& rc =
            backup_slot_refcount_[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                                  ba.wavelength];
        TON_ASSERT(rc < 0xFFFFu, "backup_slot_refcount overflow");
        ++rc;
        backup_active_mask_[a] |= bit;
      }
      tbl.assignments[static_cast<size_t>(f) * tbl.per_state_cap +
                      tbl.counts[f]++] = ba;
      continue;
    }

    // DeltaKind::ReplaceState
    BackupAssignment* row =
        tbl.assignments.data() + static_cast<size_t>(f) * tbl.per_state_cap;
    const uint16_t old_cnt = tbl.counts[f];

    std::array<uint64_t, kFastDirtyArcLimit / 64> dirty_arc_words{};
    std::array<BackupAssignment, 128> old_fallback{};
    const bool use_fast_dirty = num_arcs_ <= kFastDirtyArcLimit;

    for (uint16_t i = 0; i < old_cnt; ++i) {
      const BackupAssignment old_ba = row[i];
      if (!use_fast_dirty) {
        old_fallback[i] = old_ba;
      }
      const WavelengthMask old_bit = wavelength_bit(old_ba.wavelength);
      for (const ArcId a : path_pool.path_arcs(old_ba.path_id)) {
        WavelengthMask& sm = tbl.masks[static_cast<size_t>(f) * num_arcs_ + a];
        TON_ASSERT((sm & old_bit) != 0u,
                   "Old backup bit missing in state mask during ReplaceState");
        sm &= ~old_bit;
        uint16_t& rc =
            backup_slot_refcount_[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                                  old_ba.wavelength];
        TON_ASSERT(rc > 0u, "backup_slot_refcount underflow");
        --rc;
        if (rc == 0u) {
          backup_active_mask_[a] &= ~old_bit;
        }
        if (use_fast_dirty) {
          dirty_arc_words[a >> 6] |= (uint64_t{1} << (a & 63u));
        }
      }
    }

    const auto new_entries = staged.delta_entries(d);
    TON_BOUNDS_CHECK(new_entries.size() <= tbl.per_state_cap,
                     "State backup capacity exceeded on ReplaceState");
    tbl.counts[f] = static_cast<uint16_t>(new_entries.size());

    for (size_t i = 0; i < new_entries.size(); ++i) {
      const BackupAssignment ba = new_entries[i];
      TON_ASSERT(ba.valid(), "Invalid BackupAssignment in ReplaceState");
      row[i] = ba;
      const WavelengthMask bit = wavelength_bit(ba.wavelength);
      for (const ArcId a : path_pool.path_arcs(ba.path_id)) {
        TON_BOUNDS_CHECK(a < num_arcs_, "Backup ArcId out of bounds");
        WavelengthMask& sm = tbl.masks[static_cast<size_t>(f) * num_arcs_ + a];
        TON_ASSERT((sm & bit) == 0u,
                   "Backup wavelength clash within ReplaceState");
        sm |= bit;
        tbl.block_union_masks[static_cast<size_t>(a) * tbl.num_blocks + blk] |=
            bit;
        uint16_t& rc =
            backup_slot_refcount_[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                                  ba.wavelength];
        TON_ASSERT(rc < 0xFFFFu, "backup_slot_refcount overflow");
        ++rc;
        backup_active_mask_[a] |= bit;
      }
    }

    if (use_fast_dirty) {
      for (size_t w_idx = 0; w_idx < dirty_arc_words.size(); ++w_idx) {
        uint64_t word = dirty_arc_words[w_idx];
        while (word != 0u) {
          const auto bit_pos = static_cast<uint16_t>(std::countr_zero(word));
          word &= (word - 1u);
          const auto a = static_cast<ArcId>(w_idx * 64u + bit_pos);
          refresh_block_union(tbl, blk, a);
        }
      }
    } else {
      for (uint16_t i = 0; i < old_cnt; ++i) {
        for (const ArcId a : path_pool.path_arcs(old_fallback[i].path_id)) {
          refresh_block_union(tbl, blk, a);
        }
      }
    }
  }
}

}  // namespace ton
