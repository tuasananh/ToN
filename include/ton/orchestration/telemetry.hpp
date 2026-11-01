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

  // Standardized 23-column master summary CSV format (PLAN.md Section 5.1 &
  // PHASE-5.md Section 3.2).
  static void write_summary_csv_header(std::ostream& os);
  static void write_summary_csv_row(
      std::ostream& os, std::string_view exp, std::string_view topology,
      std::string_view cut_model, std::string_view traffic,
      std::string_view seq_id, AlgorithmId algorithm, FailureRegime regime,
      std::string_view heuristic, uint32_t first_denial_idx,
      uint32_t total_granted, double blocking_prob,
      double mean_latency_pre_denial_us, double p95_latency_pre_denial_us,
      double mean_latency_full_us, double mean_latency_rejected_us,
      double u_prim_at_denial, double u_pure_at_denial,
      double overhead_at_denial, double u_prim_final, double u_pure_final,
      double u_stub_final, double overhead_final, bool invariants_verified);

  static std::string format_summary_csv_row(
      std::string_view exp, std::string_view topology,
      std::string_view cut_model, std::string_view traffic,
      std::string_view seq_id, AlgorithmId algorithm, FailureRegime regime,
      std::string_view heuristic, uint32_t first_denial_idx,
      uint32_t total_granted, double blocking_prob,
      double mean_latency_pre_denial_us, double p95_latency_pre_denial_us,
      double mean_latency_full_us, double mean_latency_rejected_us,
      double u_prim_at_denial, double u_pure_at_denial,
      double overhead_at_denial, double u_prim_final, double u_pure_final,
      double u_stub_final, double overhead_final, bool invariants_verified);

  static void append_summary_csv_row(
      const std::filesystem::path& csv_path, std::string_view exp,
      std::string_view topology, std::string_view cut_model,
      std::string_view traffic, std::string_view seq_id, AlgorithmId algorithm,
      FailureRegime regime, std::string_view heuristic,
      uint32_t first_denial_idx, uint32_t total_granted, double blocking_prob,
      double mean_latency_pre_denial_us, double p95_latency_pre_denial_us,
      double mean_latency_full_us, double mean_latency_rejected_us,
      double u_prim_at_denial, double u_pure_at_denial,
      double overhead_at_denial, double u_prim_final, double u_pure_final,
      double u_stub_final, double overhead_final, bool invariants_verified);
};

inline constexpr std::string_view MASTER_SUMMARY_CSV_HEADER =
    "exp,topology,cut_model,traffic,seq_id,algorithm,regime,heuristic,"
    "first_denial_idx,total_granted,blocking_prob,mean_latency_pre_denial_us,"
    "p95_latency_pre_denial_us,mean_latency_full_us,mean_latency_rejected_us,"
    "u_prim_at_denial,u_pure_at_denial,overhead_at_denial,u_prim_final,"
    "u_pure_final,u_stub_final,overhead_final,invariants_verified\n";

}  // namespace ton
