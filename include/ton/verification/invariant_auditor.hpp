#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"
#include "ton/orchestration/network_state.hpp"
#include "ton/orchestration/telemetry.hpp"

namespace ton {

enum class InvariantCode : uint8_t {
  INV1_PrimaryClash = 0,
  INV1_BackupClash = 1,
  INV1_IllegalCollision = 2,
  INV2_WavelengthDiscontinuity = 3,
  INV3_FalseBackup = 4,
  INV3_MissedBackup = 5,
  INV4_CutArcTraversed = 6,
  INV4_RouteAdmissibility = 7,
  INV5_SequentialDesync = 8,
  INV5_BrokenSequentialLock = 9,
  INV_SpectralRefcountDesync = 10,
  INV_SpectralActiveMaskDesync = 11,
  INV_MetricRangeViolation = 12,
  INV_MetricConsistency = 13,
};

[[nodiscard]] constexpr std::string_view invariant_code_name(
    InvariantCode code) noexcept {
  switch (code) {
    case InvariantCode::INV1_PrimaryClash:
      return "INV-1 Primary Clash";
    case InvariantCode::INV1_BackupClash:
      return "INV-1 Backup Clash";
    case InvariantCode::INV1_IllegalCollision:
      return "INV-1 Illegal Collision";
    case InvariantCode::INV2_WavelengthDiscontinuity:
      return "INV-2 Wavelength Discontinuity";
    case InvariantCode::INV3_FalseBackup:
      return "INV-3 False Backup";
    case InvariantCode::INV3_MissedBackup:
      return "INV-3 Missed Backup";
    case InvariantCode::INV4_CutArcTraversed:
      return "INV-4 Cut Arc Traversed";
    case InvariantCode::INV4_RouteAdmissibility:
      return "INV-4 Route Admissibility";
    case InvariantCode::INV5_SequentialDesync:
      return "INV-5 Sequential Desync";
    case InvariantCode::INV5_BrokenSequentialLock:
      return "INV-5 Broken Sequential Lock";
    case InvariantCode::INV_SpectralRefcountDesync:
      return "Spectral Refcount Desync";
    case InvariantCode::INV_SpectralActiveMaskDesync:
      return "Spectral Active Mask Desync";
    case InvariantCode::INV_MetricRangeViolation:
      return "Metric Range Violation";
    case InvariantCode::INV_MetricConsistency:
      return "Metric Consistency Violation";
  }
  return "Unknown";
}

struct InvariantViolation {
  InvariantCode code = InvariantCode::INV1_PrimaryClash;
  RequestId req_id = INVALID_REQUEST;
  ArcId arc_id = INVALID_ARC;
  uint8_t wavelength = INVALID_WAVELENGTH;
  uint32_t state_id = INVALID_SCENARIO;
  StateDomain domain = StateDomain::Sim;
  std::string message;
};

struct AuditResult {
  bool passed = true;
  std::vector<InvariantViolation> violations;
  SpectralMetrics evaluated_metrics{};
  uint32_t audited_admitted_requests = 0;
  uint32_t audited_failure_states = 0;

  [[nodiscard]] bool has_violation(InvariantCode code) const noexcept {
    for (const auto& v : violations) {
      if (v.code == code) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] size_t count_violations(InvariantCode code) const noexcept {
    size_t cnt = 0;
    for (const auto& v : violations) {
      if (v.code == code) {
        ++cnt;
      }
    }
    return cnt;
  }
};

class InvariantAuditor {
 public:
  // Exhaustive independent full-state checker sharing zero fast bitmask
  // shortcuts with NetworkState. Brute-force verifies System Invariants INV-1
  // through INV-5 and asserts bit-for-bit parity with backup_slot_refcount and
  // backup_active_mask.
  // max_violations: if > 0, stops collecting after reaching max_violations.
  [[nodiscard]] static AuditResult audit_state(const NetworkState& state,
                                               const PathPool& pool,
                                               size_t max_violations = 0);

  // Audits a single step record against metric invariants:
  // 0 <= U_prim, U_back, U_total <= 100%, U_total = U_prim + U_pure,
  // rho_overhead >= 0.
  [[nodiscard]] static AuditResult audit_step_record(const StepRecord& record,
                                                     double eps = 1e-5);

  // Audits an entire step trajectory CSV log against metric invariants.
  [[nodiscard]] static AuditResult audit_trajectory_csv(
      const std::filesystem::path& csv_path, double eps = 1e-5);
};

}  // namespace ton
