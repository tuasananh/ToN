#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>

#include "ton/core/types.hpp"
#include "ton/orchestration/network_state.hpp"

namespace ton {

struct SpectralMetrics {
  uint32_t h_prim_slots = 0;
  uint32_t h_back_slots = 0;
  uint32_t h_pure_slots = 0;
  uint32_t h_stub_slots = 0;
  double u_prim_pct = 0.0;
  double u_back_pct = 0.0;
  double u_pure_pct = 0.0;
  double u_stub_pct = 0.0;
  double u_total_pct = 0.0;
  double overhead_ratio = 0.0;

  [[nodiscard]] bool operator==(const SpectralMetrics&) const noexcept =
      default;
};

struct StepRecord {
  uint32_t t = 0;
  NodeId src = INVALID_NODE;
  NodeId dst = INVALID_NODE;
  bool admitted = false;
  double latency_us = 0.0;
  uint32_t k_adm = 0;
  SpectralMetrics spectral{};
};

class SpectralTelemetry {
 public:
  // Evaluates all 6 exact spectral metrics (U_prim, U_back, U_pure, U_stub,
  // U_total, rho_overhead) in < 0.5 us by scanning L1-resident primary_mask and
  // backup_slot_refcount[M][W].
  [[nodiscard]] static SpectralMetrics evaluate(
      const NetworkState& state) noexcept;

  // Independent full-state scan over all scenario bitmasks (Simultaneous F_sim
  // or Sequential Stage-1 E_1 union Stage-2 F_seq), sharing zero helper logic
  // with backup_slot_refcount.
  [[nodiscard]] static SpectralMetrics evaluate_full_scan(
      const NetworkState& state) noexcept;

  // Verifies bit-for-bit and slot-for-slot equality between
  // backup_slot_refcount[M][W] and the exact count of failure states reserving
  // each (arc, wavelength) slot across all scenario masks.
  [[nodiscard]] static bool verify_refcounts_exact(
      const NetworkState& state) noexcept;

  // Standardized per-step trajectory CSV writer (PLAN.md Section 5.1).
  static void write_csv_header(std::ostream& os);
  static void write_csv_row(std::ostream& os, const StepRecord& record);
  static void write_step_records_csv(const std::filesystem::path& csv_path,
                                     std::span<const StepRecord> records);
};

}  // namespace ton
