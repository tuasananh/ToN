#include "ton/verification/invariant_auditor.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace ton {

namespace {

[[nodiscard]] bool check_path_validity(const Graph& graph, const PathPool& pool,
                                       PathId pid, NodeId src, NodeId dst,
                                       std::string& err_msg) {
  if (pid == INVALID_PATH || pid >= pool.num_paths()) {
    err_msg = "Invalid PathId (" + std::to_string(pid) + ")";
    return false;
  }

  const auto arcs = pool.path_arcs(pid);
  const auto nodes = pool.path_nodes(pid);

  if (arcs.empty() || nodes.empty()) {
    err_msg = "Empty route";
    return false;
  }

  if (nodes.front() != src || nodes.back() != dst) {
    err_msg = "Route endpoints (" + std::to_string(nodes.front()) + " -> " +
              std::to_string(nodes.back()) + ") do not match request (" +
              std::to_string(src) + " -> " + std::to_string(dst) + ")";
    return false;
  }

  if (nodes.size() != arcs.size() + 1u) {
    err_msg = "Node and arc counts inconsistent";
    return false;
  }

  // Verify contiguous walk
  for (size_t i = 0; i < arcs.size(); ++i) {
    const ArcId a = arcs[i];
    if (a >= graph.num_arcs()) {
      err_msg = "ArcId out of graph bounds (" + std::to_string(a) + ")";
      return false;
    }
    const auto& arc_data = graph.arc(a);
    if (arc_data.src != nodes[i] || arc_data.dst != nodes[i + 1u]) {
      err_msg = "Discontinuous walk at hop " + std::to_string(i);
      return false;
    }
  }

  // Verify loop-freedom (simple path)
  for (size_t i = 0; i < nodes.size(); ++i) {
    for (size_t j = i + 1; j < nodes.size(); ++j) {
      if (nodes[i] == nodes[j]) {
        err_msg = "Route contains loop at node " + std::to_string(nodes[i]);
        return false;
      }
    }
  }

  return true;
}

[[nodiscard]] bool is_path_in_candidate_slice(const PathPool& pool,
                                              PathSlice slice, PathId pid) {
  if (slice.empty()) {
    return false;
  }
  const auto target_arcs = pool.path_arcs(pid);
  for (const PathId cand_pid : slice) {
    if (cand_pid == pid) {
      return true;
    }
    const auto cand_arcs = pool.path_arcs(cand_pid);
    if (cand_arcs.size() == target_arcs.size() &&
        std::equal(cand_arcs.begin(), cand_arcs.end(), target_arcs.begin())) {
      return true;
    }
  }
  return false;
}

}  // namespace

AuditResult InvariantAuditor::audit_state(const NetworkState& state,
                                          const PathPool& pool,
                                          size_t max_violations) {
  AuditResult result{};
  auto record_violation = [&](InvariantCode code, RequestId r, ArcId a,
                              uint8_t w, uint32_t state_id, StateDomain domain,
                              std::string msg) {
    result.passed = false;
    result.violations.push_back(InvariantViolation{
        .code = code,
        .req_id = r,
        .arc_id = a,
        .wavelength = w,
        .state_id = state_id,
        .domain = domain,
        .message = std::move(msg),
    });
  };

  auto should_stop = [&]() -> bool {
    return max_violations > 0 && result.violations.size() >= max_violations;
  };

  const auto& graph = state.graph();
  const auto& scenarios = state.scenarios();
  const ArcId num_arcs = state.num_arcs();
  const size_t max_requests = state.max_requests();
  const auto admitted_ids = state.admitted_ids();
  result.audited_admitted_requests = static_cast<uint32_t>(admitted_ids.size());

  // 1. Audit Primary Allocations (INV-1, INV-2, INV-4)
  std::vector<RequestId> primary_occupants(
      static_cast<size_t>(num_arcs) * NUM_WAVELENGTHS, INVALID_REQUEST);
  std::vector<uint8_t> is_admitted_map(max_requests, 0);

  for (const RequestId r : admitted_ids) {
    if (should_stop()) {
      break;
    }
    if (r >= max_requests) {
      record_violation(
          InvariantCode::INV4_RouteAdmissibility, r, INVALID_ARC,
          INVALID_WAVELENGTH, INVALID_SCENARIO, StateDomain::Sim,
          "RequestId " + std::to_string(r) + " exceeds max_requests");
      continue;
    }
    if (!state.is_admitted(r)) {
      record_violation(
          InvariantCode::INV4_RouteAdmissibility, r, INVALID_ARC,
          INVALID_WAVELENGTH, INVALID_SCENARIO, StateDomain::Sim,
          "RequestId " + std::to_string(r) +
              " listed in admitted_ids but is_admitted() is false");
      continue;
    }
    is_admitted_map[r] = 1;

    const Request& req = state.request(r);
    const Lightpath prim = state.primary_lightpath(r);

    // INV-2: Wavelength validity
    if (prim.wavelength >= NUM_WAVELENGTHS) {
      record_violation(InvariantCode::INV2_WavelengthDiscontinuity, r,
                       INVALID_ARC, prim.wavelength, INVALID_SCENARIO,
                       StateDomain::Sim,
                       "Primary wavelength out of range [0, 31]");
    }

    // INV-4: Route validity and loop-freedom
    std::string path_err;
    if (!check_path_validity(graph, pool, prim.path_id, req.src, req.dst,
                             path_err)) {
      record_violation(InvariantCode::INV4_RouteAdmissibility, r, INVALID_ARC,
                       prim.wavelength, INVALID_SCENARIO, StateDomain::Sim,
                       "Primary route invalid: " + path_err);
    } else {
      const PathSlice healthy_slice = pool.find_healthy(req.src, req.dst);
      if (!is_path_in_candidate_slice(pool, healthy_slice, prim.path_id)) {
        record_violation(
            InvariantCode::INV4_RouteAdmissibility, r, INVALID_ARC,
            prim.wavelength, INVALID_SCENARIO, StateDomain::Sim,
            "Primary route not found in candidate healthy path pool");
      }
    }

    // INV-1: Primary wavelength clash & spatial exclusion
    if (prim.path_id != INVALID_PATH && prim.path_id < pool.num_paths() &&
        prim.wavelength < NUM_WAVELENGTHS) {
      for (const ArcId a : pool.path_arcs(prim.path_id)) {
        if (a >= num_arcs) {
          record_violation(InvariantCode::INV4_RouteAdmissibility, r, a,
                           prim.wavelength, INVALID_SCENARIO, StateDomain::Sim,
                           "Primary arc ID out of bounds");
          continue;
        }
        const size_t idx =
            static_cast<size_t>(a) * NUM_WAVELENGTHS + prim.wavelength;
        if (primary_occupants[idx] != INVALID_REQUEST) {
          record_violation(InvariantCode::INV1_PrimaryClash, r, a,
                           prim.wavelength, INVALID_SCENARIO, StateDomain::Sim,
                           "Primary clash with Request " +
                               std::to_string(primary_occupants[idx]) +
                               " on slot (" + std::to_string(a) + ", " +
                               std::to_string(prim.wavelength) + ")");
        } else {
          primary_occupants[idx] = r;
        }

        // Primary owner consistency
        if (state.primary_owner(a, prim.wavelength) != r) {
          record_violation(
              InvariantCode::INV1_PrimaryClash, r, a, prim.wavelength,
              INVALID_SCENARIO, StateDomain::Sim,
              "state.primary_owner (" +
                  std::to_string(state.primary_owner(a, prim.wavelength)) +
                  ") does not match request " + std::to_string(r));
        }

        // Check for hop-level wavelength desync (no other wavelength claimed on
        // this arc)
        for (uint8_t w = 0; w < NUM_WAVELENGTHS; ++w) {
          if (w != prim.wavelength && state.primary_owner(a, w) == r) {
            record_violation(InvariantCode::INV2_WavelengthDiscontinuity, r, a,
                             w, INVALID_SCENARIO, StateDomain::Sim,
                             "Request has inconsistent wavelength on arc " +
                                 std::to_string(a) + ": primary is " +
                                 std::to_string(prim.wavelength) +
                                 " but owner table has " + std::to_string(w));
          }
        }
      }
    }
  }

  // Cross-check primary masks
  for (ArcId a = 0; a < num_arcs; ++a) {
    if (should_stop()) {
      break;
    }
    WavelengthMask expected_mask = EMPTY_MASK;
    for (uint8_t w = 0; w < NUM_WAVELENGTHS; ++w) {
      const RequestId occ =
          primary_occupants[static_cast<size_t>(a) * NUM_WAVELENGTHS + w];
      if (occ != INVALID_REQUEST) {
        expected_mask |= wavelength_bit(w);
      } else {
        if (state.primary_owner(a, w) != INVALID_REQUEST) {
          record_violation(
              InvariantCode::INV1_PrimaryClash, state.primary_owner(a, w), a, w,
              INVALID_SCENARIO, StateDomain::Sim,
              "Unallocated primary slot (" + std::to_string(a) + ", " +
                  std::to_string(w) + ") has active primary_owner");
        }
      }
    }
    if (expected_mask != state.primary_mask(a)) {
      record_violation(
          InvariantCode::INV1_PrimaryClash, INVALID_REQUEST, a,
          INVALID_WAVELENGTH, INVALID_SCENARIO, StateDomain::Sim,
          "state.primary_mask mismatch on arc " + std::to_string(a));
    }
  }

  // 2. Audit Backup Allocations across all Failure States
  std::vector<uint32_t> independent_refcounts(
      static_cast<size_t>(num_arcs) * NUM_WAVELENGTHS, 0u);
  std::vector<RequestId> scen_backup_occupants(
      static_cast<size_t>(num_arcs) * NUM_WAVELENGTHS, INVALID_REQUEST);
  std::vector<uint8_t> seen_backup(max_requests, 0);
  std::vector<uint8_t> is_disrupted(max_requests, 0);
  std::vector<uint8_t> slot_used_in_state(
      static_cast<size_t>(num_arcs) * NUM_WAVELENGTHS, 0);

  if (state.regime() == FailureRegime::Simultaneous) {
    const auto sim_scenarios = scenarios.sim_scenarios();
    result.audited_failure_states = static_cast<uint32_t>(sim_scenarios.size());

    for (const auto& sc : sim_scenarios) {
      if (should_stop()) {
        break;
      }
      std::fill(scen_backup_occupants.begin(), scen_backup_occupants.end(),
                INVALID_REQUEST);
      std::fill(seen_backup.begin(), seen_backup.end(), uint8_t{0});
      std::fill(is_disrupted.begin(), is_disrupted.end(), uint8_t{0});
      std::fill(slot_used_in_state.begin(), slot_used_in_state.end(),
                uint8_t{0});

      // Ground truth disruption
      for (const RequestId r : admitted_ids) {
        const Lightpath prim = state.primary_lightpath(r);
        if (pool.path_intersects(prim.path_id, sc.cut_arcs)) {
          is_disrupted[r] = 1;
        }
      }

      const auto backups = state.sim_backups(sc.id);
      for (const auto& ba : backups) {
        if (!ba.valid()) {
          record_violation(InvariantCode::INV4_RouteAdmissibility, ba.req_id,
                           INVALID_ARC, ba.wavelength, sc.id, StateDomain::Sim,
                           "Invalid BackupAssignment");
          continue;
        }
        if (ba.req_id >= max_requests || !is_admitted_map[ba.req_id]) {
          record_violation(InvariantCode::INV3_FalseBackup, ba.req_id,
                           INVALID_ARC, ba.wavelength, sc.id, StateDomain::Sim,
                           "Backup active for non-admitted request");
          continue;
        }
        if (!is_disrupted[ba.req_id]) {
          record_violation(
              InvariantCode::INV3_FalseBackup, ba.req_id, INVALID_ARC,
              ba.wavelength, sc.id, StateDomain::Sim,
              "Backup active for request whose primary was NOT severed");
        }
        if (++seen_backup[ba.req_id] > 1) {
          record_violation(InvariantCode::INV1_BackupClash, ba.req_id,
                           INVALID_ARC, ba.wavelength, sc.id, StateDomain::Sim,
                           "Multiple backup lightpaths in same scenario");
        }

        // INV-2: Wavelength range
        if (ba.wavelength >= NUM_WAVELENGTHS) {
          record_violation(InvariantCode::INV2_WavelengthDiscontinuity,
                           ba.req_id, INVALID_ARC, ba.wavelength, sc.id,
                           StateDomain::Sim, "Backup wavelength out of range");
          continue;
        }

        const Request& req = state.request(ba.req_id);

        // INV-4: Route validity
        std::string path_err;
        if (!check_path_validity(graph, pool, ba.path_id, req.src, req.dst,
                                 path_err)) {
          record_violation(InvariantCode::INV4_RouteAdmissibility, ba.req_id,
                           INVALID_ARC, ba.wavelength, sc.id, StateDomain::Sim,
                           "Backup route invalid: " + path_err);
          if (ba.path_id >= pool.num_paths()) {
            continue;
          }
        } else {
          // INV-4: Severed arc traversal
          if (pool.path_intersects(ba.path_id, sc.cut_arcs)) {
            record_violation(InvariantCode::INV4_CutArcTraversed, ba.req_id,
                             INVALID_ARC, ba.wavelength, sc.id,
                             StateDomain::Sim,
                             "Backup route intersects severed cut arcs L(F)");
          }

          // Candidate pool membership
          const PathSlice cand_slice = pool.find(sc.cut_arcs, req.src, req.dst);
          if (!is_path_in_candidate_slice(pool, cand_slice, ba.path_id)) {
            record_violation(
                InvariantCode::INV4_RouteAdmissibility, ba.req_id, INVALID_ARC,
                ba.wavelength, sc.id, StateDomain::Sim,
                "Backup route not in candidate path pool for surviving graph");
          }
        }

        // INV-1: Backup clash & Illegal collision with intact primary
        for (const ArcId a : pool.path_arcs(ba.path_id)) {
          if (a >= num_arcs) {
            continue;
          }
          const size_t slot_idx =
              static_cast<size_t>(a) * NUM_WAVELENGTHS + ba.wavelength;

          if (scen_backup_occupants[slot_idx] != INVALID_REQUEST) {
            record_violation(
                InvariantCode::INV1_BackupClash, ba.req_id, a, ba.wavelength,
                sc.id, StateDomain::Sim,
                "Backup clash with request " +
                    std::to_string(scen_backup_occupants[slot_idx]) +
                    " on slot (" + std::to_string(a) + ", " +
                    std::to_string(ba.wavelength) + ")");
          } else {
            scen_backup_occupants[slot_idx] = ba.req_id;
          }

          // Collision with intact primary
          const RequestId prim_owner = primary_occupants[slot_idx];
          if (prim_owner != INVALID_REQUEST) {
            if (!is_disrupted[prim_owner]) {
              record_violation(
                  InvariantCode::INV1_IllegalCollision, ba.req_id, a,
                  ba.wavelength, sc.id, StateDomain::Sim,
                  "Backup collided with intact primary of Request " +
                      std::to_string(prim_owner));
            }
          }

          slot_used_in_state[slot_idx] = 1;
        }
      }

      // INV-3: Missed backups
      for (const RequestId r : admitted_ids) {
        if (is_disrupted[r] && seen_backup[r] == 0) {
          record_violation(
              InvariantCode::INV3_MissedBackup, r, INVALID_ARC,
              INVALID_WAVELENGTH, sc.id, StateDomain::Sim,
              "Request severed by cut arcs but has no restoration backup");
        }
      }

      // Cross-check sim_backup_mask for scenario
      for (ArcId a = 0; a < num_arcs; ++a) {
        WavelengthMask expected_mask = EMPTY_MASK;
        for (uint8_t w = 0; w < NUM_WAVELENGTHS; ++w) {
          if (slot_used_in_state[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                                 w]) {
            expected_mask |= wavelength_bit(w);
          }
        }
        if (expected_mask != state.sim_backup_mask(sc.id, a)) {
          record_violation(InvariantCode::INV1_BackupClash, INVALID_REQUEST, a,
                           INVALID_WAVELENGTH, sc.id, StateDomain::Sim,
                           "state.sim_backup_mask mismatch in scenario " +
                               std::to_string(sc.id));
        }
      }

      // Accumulate independent refcount
      for (size_t slot = 0; slot < slot_used_in_state.size(); ++slot) {
        if (slot_used_in_state[slot]) {
          ++independent_refcounts[slot];
        }
      }
    }
  } else {
    // Sequential Failure Regime
    const uint32_t num_units = scenarios.num_units();
    const auto active_s1 = scenarios.active_stage1_units();
    const auto seq_scenarios = scenarios.seq_scenarios();
    result.audited_failure_states =
        static_cast<uint32_t>(active_s1.size() + seq_scenarios.size());

    // Inactive Stage-1 units must have zero backups
    for (FailureUnitId u = 0; u < num_units; ++u) {
      if (!scenarios.is_active_stage1_unit(u)) {
        if (!state.seq_stage1_backups(u).empty()) {
          record_violation(
              InvariantCode::INV5_SequentialDesync, INVALID_REQUEST,
              INVALID_ARC, INVALID_WAVELENGTH, u, StateDomain::SeqStage1,
              "Inactive Stage-1 failure unit has allocated backups");
        }
      }
    }

    // Cache Stage-1 allocations for INV-5 locked backup validation
    std::vector<Lightpath> s1_backup_by_unit_and_req(
        static_cast<size_t>(num_units) * max_requests, Lightpath{});

    // 2A. Audit Stage-1 Units
    for (const FailureUnitId x1 : active_s1) {
      if (should_stop()) {
        break;
      }
      const CutArcs& s1_cut = scenarios.unit_cut_arcs(x1);
      std::fill(scen_backup_occupants.begin(), scen_backup_occupants.end(),
                INVALID_REQUEST);
      std::fill(seen_backup.begin(), seen_backup.end(), uint8_t{0});
      std::fill(is_disrupted.begin(), is_disrupted.end(), uint8_t{0});
      std::fill(slot_used_in_state.begin(), slot_used_in_state.end(),
                uint8_t{0});

      for (const RequestId r : admitted_ids) {
        const Lightpath prim = state.primary_lightpath(r);
        if (pool.path_intersects(prim.path_id, s1_cut)) {
          is_disrupted[r] = 1;
        }
      }

      const auto s1_backups = state.seq_stage1_backups(x1);
      for (const auto& ba : s1_backups) {
        if (!ba.valid()) {
          record_violation(InvariantCode::INV4_RouteAdmissibility, ba.req_id,
                           INVALID_ARC, ba.wavelength, x1,
                           StateDomain::SeqStage1, "Invalid BackupAssignment");
          continue;
        }
        if (ba.req_id >= max_requests || !is_admitted_map[ba.req_id]) {
          record_violation(InvariantCode::INV3_FalseBackup, ba.req_id,
                           INVALID_ARC, ba.wavelength, x1,
                           StateDomain::SeqStage1,
                           "Stage-1 backup active for non-admitted request");
          continue;
        }
        if (!is_disrupted[ba.req_id]) {
          record_violation(InvariantCode::INV3_FalseBackup, ba.req_id,
                           INVALID_ARC, ba.wavelength, x1,
                           StateDomain::SeqStage1,
                           "Stage-1 backup active for request whose primary "
                           "was NOT severed");
        }
        if (++seen_backup[ba.req_id] > 1) {
          record_violation(InvariantCode::INV1_BackupClash, ba.req_id,
                           INVALID_ARC, ba.wavelength, x1,
                           StateDomain::SeqStage1,
                           "Multiple Stage-1 backups for same request");
        }

        if (ba.wavelength >= NUM_WAVELENGTHS) {
          record_violation(InvariantCode::INV2_WavelengthDiscontinuity,
                           ba.req_id, INVALID_ARC, ba.wavelength, x1,
                           StateDomain::SeqStage1,
                           "Stage-1 backup wavelength out of range");
          continue;
        }

        const Request& req = state.request(ba.req_id);

        std::string path_err;
        if (!check_path_validity(graph, pool, ba.path_id, req.src, req.dst,
                                 path_err)) {
          record_violation(InvariantCode::INV4_RouteAdmissibility, ba.req_id,
                           INVALID_ARC, ba.wavelength, x1,
                           StateDomain::SeqStage1,
                           "Stage-1 backup route invalid: " + path_err);
          if (ba.path_id >= pool.num_paths()) {
            continue;
          }
        } else {
          if (pool.path_intersects(ba.path_id, s1_cut)) {
            record_violation(InvariantCode::INV4_CutArcTraversed, ba.req_id,
                             INVALID_ARC, ba.wavelength, x1,
                             StateDomain::SeqStage1,
                             "Stage-1 backup traverses severed arcs L_cut(x1)");
          }

          const PathSlice cand_slice = pool.find(s1_cut, req.src, req.dst);
          if (!is_path_in_candidate_slice(pool, cand_slice, ba.path_id)) {
            record_violation(InvariantCode::INV4_RouteAdmissibility, ba.req_id,
                             INVALID_ARC, ba.wavelength, x1,
                             StateDomain::SeqStage1,
                             "Stage-1 backup route not in candidate path pool");
          }
        }

        s1_backup_by_unit_and_req[static_cast<size_t>(x1) * max_requests +
                                  ba.req_id] = ba.lightpath();

        for (const ArcId a : pool.path_arcs(ba.path_id)) {
          if (a >= num_arcs) {
            continue;
          }
          const size_t slot_idx =
              static_cast<size_t>(a) * NUM_WAVELENGTHS + ba.wavelength;

          if (scen_backup_occupants[slot_idx] != INVALID_REQUEST) {
            record_violation(
                InvariantCode::INV1_BackupClash, ba.req_id, a, ba.wavelength,
                x1, StateDomain::SeqStage1,
                "Stage-1 backup clash with request " +
                    std::to_string(scen_backup_occupants[slot_idx]));
          } else {
            scen_backup_occupants[slot_idx] = ba.req_id;
          }

          const RequestId prim_owner = primary_occupants[slot_idx];
          if (prim_owner != INVALID_REQUEST) {
            if (!is_disrupted[prim_owner]) {
              record_violation(
                  InvariantCode::INV1_IllegalCollision, ba.req_id, a,
                  ba.wavelength, x1, StateDomain::SeqStage1,
                  "Stage-1 backup collided with intact primary of Request " +
                      std::to_string(prim_owner));
            }
          }

          slot_used_in_state[slot_idx] = 1;
        }
      }

      for (const RequestId r : admitted_ids) {
        if (is_disrupted[r] && seen_backup[r] == 0) {
          record_violation(InvariantCode::INV3_MissedBackup, r, INVALID_ARC,
                           INVALID_WAVELENGTH, x1, StateDomain::SeqStage1,
                           "Stage-1 severed request missing backup");
        }
      }

      // Cross-check seq_stage1_backup_mask
      for (ArcId a = 0; a < num_arcs; ++a) {
        WavelengthMask expected_mask = EMPTY_MASK;
        for (uint8_t w = 0; w < NUM_WAVELENGTHS; ++w) {
          if (slot_used_in_state[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                                 w]) {
            expected_mask |= wavelength_bit(w);
          }
        }
        if (expected_mask != state.seq_stage1_backup_mask(x1, a)) {
          record_violation(InvariantCode::INV1_BackupClash, INVALID_REQUEST, a,
                           INVALID_WAVELENGTH, x1, StateDomain::SeqStage1,
                           "state.seq_stage1_backup_mask mismatch for unit " +
                               std::to_string(x1));
        }
      }

      for (size_t slot = 0; slot < slot_used_in_state.size(); ++slot) {
        if (slot_used_in_state[slot]) {
          ++independent_refcounts[slot];
        }
      }
    }

    // 2B. Audit Stage-2 Scenarios
    for (const auto& sc : seq_scenarios) {
      if (should_stop()) {
        break;
      }
      std::fill(scen_backup_occupants.begin(), scen_backup_occupants.end(),
                INVALID_REQUEST);
      std::fill(seen_backup.begin(), seen_backup.end(), uint8_t{0});
      std::fill(is_disrupted.begin(), is_disrupted.end(), uint8_t{0});
      std::fill(slot_used_in_state.begin(), slot_used_in_state.end(),
                uint8_t{0});

      for (const RequestId r : admitted_ids) {
        const Lightpath prim = state.primary_lightpath(r);
        if (pool.path_intersects(prim.path_id, sc.total_cut)) {
          is_disrupted[r] = 1;
        }
      }

      const auto s2_backups = state.seq_stage2_backups(sc.id);
      for (const auto& ba : s2_backups) {
        if (!ba.valid()) {
          record_violation(InvariantCode::INV4_RouteAdmissibility, ba.req_id,
                           INVALID_ARC, ba.wavelength, sc.id,
                           StateDomain::SeqStage2, "Invalid BackupAssignment");
          continue;
        }
        if (ba.req_id >= max_requests || !is_admitted_map[ba.req_id]) {
          record_violation(InvariantCode::INV3_FalseBackup, ba.req_id,
                           INVALID_ARC, ba.wavelength, sc.id,
                           StateDomain::SeqStage2,
                           "Stage-2 backup active for non-admitted request");
          continue;
        }
        if (!is_disrupted[ba.req_id]) {
          record_violation(InvariantCode::INV3_FalseBackup, ba.req_id,
                           INVALID_ARC, ba.wavelength, sc.id,
                           StateDomain::SeqStage2,
                           "Stage-2 backup active for request whose primary "
                           "was NOT severed");
        }
        if (++seen_backup[ba.req_id] > 1) {
          record_violation(InvariantCode::INV1_BackupClash, ba.req_id,
                           INVALID_ARC, ba.wavelength, sc.id,
                           StateDomain::SeqStage2,
                           "Multiple Stage-2 backups for same request");
        }

        if (ba.wavelength >= NUM_WAVELENGTHS) {
          record_violation(InvariantCode::INV2_WavelengthDiscontinuity,
                           ba.req_id, INVALID_ARC, ba.wavelength, sc.id,
                           StateDomain::SeqStage2,
                           "Stage-2 backup wavelength out of range");
          continue;
        }

        const Request& req = state.request(ba.req_id);
        const Lightpath prim = state.primary_lightpath(ba.req_id);
        const bool cut_in_s1 =
            pool.path_intersects(prim.path_id, sc.stage1_cut);
        bool is_locked_from_s1 = false;

        // INV-5: Sequential Stage-1 Coupling & Locked Backup Check
        if (cut_in_s1) {
          const Lightpath s1_lp =
              s1_backup_by_unit_and_req[static_cast<size_t>(sc.x1) *
                                            max_requests +
                                        ba.req_id];
          if (!s1_lp.valid()) {
            record_violation(
                InvariantCode::INV5_SequentialDesync, ba.req_id, INVALID_ARC,
                ba.wavelength, sc.id, StateDomain::SeqStage2,
                "Request severed in Stage 1 but missing cached Stage-1 backup");
          } else {
            const bool s1_survived_x2 =
                !pool.path_intersects(s1_lp.path_id, sc.stage2_cut);
            if (s1_survived_x2) {
              is_locked_from_s1 = true;
              if (ba.lightpath() != s1_lp) {
                record_violation(
                    InvariantCode::INV5_BrokenSequentialLock, ba.req_id,
                    INVALID_ARC, ba.wavelength, sc.id, StateDomain::SeqStage2,
                    "Surviving Stage-1 backup altered or relocated in Stage 2");
              }
            }
          }
        }

        std::string path_err;
        if (!check_path_validity(graph, pool, ba.path_id, req.src, req.dst,
                                 path_err)) {
          record_violation(InvariantCode::INV4_RouteAdmissibility, ba.req_id,
                           INVALID_ARC, ba.wavelength, sc.id,
                           StateDomain::SeqStage2,
                           "Stage-2 backup route invalid: " + path_err);
          if (ba.path_id >= pool.num_paths()) {
            continue;
          }
        } else {
          if (pool.path_intersects(ba.path_id, sc.total_cut)) {
            record_violation(
                InvariantCode::INV4_CutArcTraversed, ba.req_id, INVALID_ARC,
                ba.wavelength, sc.id, StateDomain::SeqStage2,
                "Stage-2 backup traverses severed arcs in total_cut");
          }

          // Candidate pool check
          const PathSlice cand_slice =
              pool.find(sc.total_cut, req.src, req.dst);
          bool in_pool =
              is_path_in_candidate_slice(pool, cand_slice, ba.path_id);
          if (!in_pool && is_locked_from_s1) {
            // Also admissible if it was valid in Stage 1 and avoids stage2_cut
            const PathSlice s1_cand =
                pool.find(sc.stage1_cut, req.src, req.dst);
            in_pool = is_path_in_candidate_slice(pool, s1_cand, ba.path_id);
          }
          if (!in_pool) {
            record_violation(
                InvariantCode::INV4_RouteAdmissibility, ba.req_id, INVALID_ARC,
                ba.wavelength, sc.id, StateDomain::SeqStage2,
                "Stage-2 backup route not found in candidate pool");
          }
        }

        for (const ArcId a : pool.path_arcs(ba.path_id)) {
          if (a >= num_arcs) {
            continue;
          }
          const size_t slot_idx =
              static_cast<size_t>(a) * NUM_WAVELENGTHS + ba.wavelength;

          if (scen_backup_occupants[slot_idx] != INVALID_REQUEST) {
            record_violation(
                InvariantCode::INV1_BackupClash, ba.req_id, a, ba.wavelength,
                sc.id, StateDomain::SeqStage2,
                "Stage-2 backup clash with request " +
                    std::to_string(scen_backup_occupants[slot_idx]));
          } else {
            scen_backup_occupants[slot_idx] = ba.req_id;
          }

          const RequestId prim_owner = primary_occupants[slot_idx];
          if (prim_owner != INVALID_REQUEST) {
            if (!is_disrupted[prim_owner]) {
              record_violation(
                  InvariantCode::INV1_IllegalCollision, ba.req_id, a,
                  ba.wavelength, sc.id, StateDomain::SeqStage2,
                  "Stage-2 backup collided with intact primary of Request " +
                      std::to_string(prim_owner));
            }
          }

          slot_used_in_state[slot_idx] = 1;
        }
      }

      for (const RequestId r : admitted_ids) {
        if (is_disrupted[r] && seen_backup[r] == 0) {
          record_violation(InvariantCode::INV3_MissedBackup, r, INVALID_ARC,
                           INVALID_WAVELENGTH, sc.id, StateDomain::SeqStage2,
                           "Stage-2 severed request missing backup");
        }
      }

      for (ArcId a = 0; a < num_arcs; ++a) {
        WavelengthMask expected_mask = EMPTY_MASK;
        for (uint8_t w = 0; w < NUM_WAVELENGTHS; ++w) {
          if (slot_used_in_state[static_cast<size_t>(a) * NUM_WAVELENGTHS +
                                 w]) {
            expected_mask |= wavelength_bit(w);
          }
        }
        if (expected_mask != state.seq_stage2_backup_mask(sc.id, a)) {
          record_violation(
              InvariantCode::INV1_BackupClash, INVALID_REQUEST, a,
              INVALID_WAVELENGTH, sc.id, StateDomain::SeqStage2,
              "state.seq_stage2_backup_mask mismatch for scenario " +
                  std::to_string(sc.id));
        }
      }

      for (size_t slot = 0; slot < slot_used_in_state.size(); ++slot) {
        if (slot_used_in_state[slot]) {
          ++independent_refcounts[slot];
        }
      }
    }
  }

  // 3. Spectral Telemetry and Refcount Invariant Verification
  const StateDomain telemetry_domain =
      (state.regime() == FailureRegime::Simultaneous) ? StateDomain::Sim
                                                      : StateDomain::SeqStage1;

  for (ArcId a = 0; a < num_arcs; ++a) {
    if (should_stop()) {
      break;
    }
    WavelengthMask expected_active = EMPTY_MASK;
    for (uint8_t w = 0; w < NUM_WAVELENGTHS; ++w) {
      const size_t slot_idx = static_cast<size_t>(a) * NUM_WAVELENGTHS + w;
      const uint32_t expected_rc = independent_refcounts[slot_idx];
      const uint16_t actual_rc = state.backup_slot_refcount(a, w);

      if (actual_rc != expected_rc) {
        record_violation(
            InvariantCode::INV_SpectralRefcountDesync, INVALID_REQUEST, a, w,
            INVALID_SCENARIO, telemetry_domain,
            "Refcount mismatch: actual=" + std::to_string(actual_rc) +
                " != expected=" + std::to_string(expected_rc));
      }
      if (expected_rc > 0u) {
        expected_active |= wavelength_bit(w);
      }
    }

    if (state.backup_active_mask(a) != expected_active) {
      record_violation(
          InvariantCode::INV_SpectralActiveMaskDesync, INVALID_REQUEST, a,
          INVALID_WAVELENGTH, INVALID_SCENARIO, telemetry_domain,
          "Backup active mask mismatch on arc " + std::to_string(a));
    }
  }

  // 4. Independent Spectral Set Union Computations
  uint32_t h_prim = 0;
  uint32_t h_back = 0;
  uint32_t h_pure = 0;
  uint32_t h_stub = 0;

  for (ArcId a = 0; a < num_arcs; ++a) {
    for (uint8_t w = 0; w < NUM_WAVELENGTHS; ++w) {
      const size_t slot_idx = static_cast<size_t>(a) * NUM_WAVELENGTHS + w;
      const bool prim_active = (primary_occupants[slot_idx] != INVALID_REQUEST);
      const bool back_active = (independent_refcounts[slot_idx] > 0u);

      if (prim_active) {
        ++h_prim;
      }
      if (back_active) {
        ++h_back;
      }
      if (prim_active && back_active) {
        ++h_stub;
      }
      if (!prim_active && back_active) {
        ++h_pure;
      }
    }
  }

  const uint32_t total_capacity =
      static_cast<uint32_t>(num_arcs) * NUM_WAVELENGTHS;
  if (total_capacity > 0u) {
    const double inv_total = 100.0 / static_cast<double>(total_capacity);
    result.evaluated_metrics = SpectralMetrics{
        .h_prim_slots = h_prim,
        .h_back_slots = h_back,
        .h_pure_slots = h_pure,
        .h_stub_slots = h_stub,
        .u_prim_pct = static_cast<double>(h_prim) * inv_total,
        .u_back_pct = static_cast<double>(h_back) * inv_total,
        .u_pure_pct = static_cast<double>(h_pure) * inv_total,
        .u_stub_pct = static_cast<double>(h_stub) * inv_total,
        .u_total_pct = static_cast<double>(h_prim + h_pure) * inv_total,
        .overhead_ratio = (h_prim > 0u) ? (static_cast<double>(h_pure) /
                                           static_cast<double>(h_prim))
                                        : 0.0,
    };
  }

  // Verify consistency with SpectralTelemetry
  const SpectralMetrics fast_metrics = SpectralTelemetry::evaluate(state);
  if (fast_metrics != result.evaluated_metrics) {
    record_violation(InvariantCode::INV_MetricConsistency, INVALID_REQUEST,
                     INVALID_ARC, INVALID_WAVELENGTH, INVALID_SCENARIO,
                     telemetry_domain,
                     "SpectralTelemetry::evaluate does not match brute-force "
                     "evaluated metrics");
  }

  return result;
}

AuditResult InvariantAuditor::audit_step_record(const StepRecord& record,
                                                double eps) {
  AuditResult result{};
  auto record_violation = [&](InvariantCode code, std::string msg) {
    result.passed = false;
    result.violations.push_back(InvariantViolation{
        .code = code,
        .req_id = static_cast<RequestId>(record.t),
        .message = std::move(msg),
    });
  };

  const auto& s = record.spectral;

  // Range checks: [0, 100%]
  if (s.u_prim_pct < -eps || s.u_prim_pct > 100.0 + eps) {
    record_violation(
        InvariantCode::INV_MetricRangeViolation,
        "u_prim_pct (" + std::to_string(s.u_prim_pct) + ") out of [0, 100%]");
  }
  if (s.u_back_pct < -eps || s.u_back_pct > 100.0 + eps) {
    record_violation(
        InvariantCode::INV_MetricRangeViolation,
        "u_back_pct (" + std::to_string(s.u_back_pct) + ") out of [0, 100%]");
  }
  if (s.u_pure_pct < -eps || s.u_pure_pct > 100.0 + eps) {
    record_violation(
        InvariantCode::INV_MetricRangeViolation,
        "u_pure_pct (" + std::to_string(s.u_pure_pct) + ") out of [0, 100%]");
  }
  if (s.u_stub_pct < -eps || s.u_stub_pct > 100.0 + eps) {
    record_violation(
        InvariantCode::INV_MetricRangeViolation,
        "u_stub_pct (" + std::to_string(s.u_stub_pct) + ") out of [0, 100%]");
  }
  if (s.u_total_pct < -eps || s.u_total_pct > 100.0 + eps) {
    record_violation(
        InvariantCode::INV_MetricRangeViolation,
        "u_total_pct (" + std::to_string(s.u_total_pct) + ") out of [0, 100%]");
  }
  if (s.overhead_ratio < -eps) {
    record_violation(
        InvariantCode::INV_MetricRangeViolation,
        "overhead_ratio (" + std::to_string(s.overhead_ratio) + ") < 0");
  }

  // Additive consistency checks
  if (std::abs(s.u_total_pct - (s.u_prim_pct + s.u_pure_pct)) > eps) {
    record_violation(InvariantCode::INV_MetricConsistency,
                     "u_total_pct != u_prim_pct + u_pure_pct");
  }
  if (std::abs(s.u_stub_pct - (s.u_back_pct - s.u_pure_pct)) > eps) {
    record_violation(InvariantCode::INV_MetricConsistency,
                     "u_stub_pct != u_back_pct - u_pure_pct");
  }
  if (s.h_back_slots != s.h_pure_slots + s.h_stub_slots) {
    record_violation(InvariantCode::INV_MetricConsistency,
                     "h_back_slots != h_pure_slots + h_stub_slots");
  }
  if (s.h_prim_slots > 0u) {
    const double expected_overhead = static_cast<double>(s.h_pure_slots) /
                                     static_cast<double>(s.h_prim_slots);
    if (std::abs(s.overhead_ratio - expected_overhead) > eps) {
      record_violation(InvariantCode::INV_MetricConsistency,
                       "overhead_ratio != h_pure_slots / h_prim_slots");
    }
  } else if (s.overhead_ratio > eps) {
    record_violation(InvariantCode::INV_MetricConsistency,
                     "overhead_ratio != 0 when h_prim_slots == 0");
  }

  result.evaluated_metrics = s;
  return result;
}

AuditResult InvariantAuditor::audit_trajectory_csv(
    const std::filesystem::path& csv_path, double eps) {
  AuditResult result{};
  std::ifstream ifs(csv_path);
  if (!ifs.is_open()) {
    result.passed = false;
    result.violations.push_back(InvariantViolation{
        .code = InvariantCode::INV_MetricRangeViolation,
        .message = "Failed to open CSV file: " + csv_path.string(),
    });
    return result;
  }

  std::string line;
  if (!std::getline(ifs, line)) {
    result.passed = false;
    result.violations.push_back(InvariantViolation{
        .code = InvariantCode::INV_MetricRangeViolation,
        .message = "Empty CSV file: " + csv_path.string(),
    });
    return result;
  }

  // Verify column header format
  if (!line.starts_with("t,src,dst,admitted,latency_us,k_adm")) {
    result.passed = false;
    result.violations.push_back(InvariantViolation{
        .code = InvariantCode::INV_MetricRangeViolation,
        .message = "Invalid or corrupted CSV header: " + line,
    });
    return result;
  }

  uint32_t row_idx = 0;
  while (std::getline(ifs, line)) {
    if (line.empty()) {
      continue;
    }

    std::string_view line_view(line);
    std::array<std::string_view, 16> tokens{};
    size_t token_count = 0;
    size_t start = 0;
    while (start <= line_view.size() && token_count < 16) {
      const size_t comma = line_view.find(',', start);
      if (comma == std::string_view::npos) {
        tokens[token_count++] = line_view.substr(start);
        break;
      }
      tokens[token_count++] = line_view.substr(start, comma - start);
      start = comma + 1;
    }

    if (token_count < 16) {
      result.passed = false;
      result.violations.push_back(InvariantViolation{
          .code = InvariantCode::INV_MetricRangeViolation,
          .req_id = static_cast<RequestId>(row_idx),
          .message =
              "Malformed row in CSV at line " + std::to_string(row_idx + 2),
      });
      ++row_idx;
      continue;
    }

    try {
      StepRecord rec{};
      rec.t = static_cast<uint32_t>(std::stoul(std::string(tokens[0])));
      rec.src = static_cast<NodeId>(std::stoul(std::string(tokens[1])));
      rec.dst = static_cast<NodeId>(std::stoul(std::string(tokens[2])));
      rec.admitted = (tokens[3] == "1");
      rec.latency_us = std::stod(std::string(tokens[4]));
      rec.k_adm = static_cast<uint32_t>(std::stoul(std::string(tokens[5])));
      rec.spectral.h_prim_slots =
          static_cast<uint32_t>(std::stoul(std::string(tokens[6])));
      rec.spectral.h_back_slots =
          static_cast<uint32_t>(std::stoul(std::string(tokens[7])));
      rec.spectral.h_pure_slots =
          static_cast<uint32_t>(std::stoul(std::string(tokens[8])));
      rec.spectral.h_stub_slots =
          static_cast<uint32_t>(std::stoul(std::string(tokens[9])));
      rec.spectral.u_prim_pct = std::stod(std::string(tokens[10]));
      rec.spectral.u_back_pct = std::stod(std::string(tokens[11]));
      rec.spectral.u_pure_pct = std::stod(std::string(tokens[12]));
      rec.spectral.u_stub_pct = std::stod(std::string(tokens[13]));
      rec.spectral.u_total_pct = std::stod(std::string(tokens[14]));
      rec.spectral.overhead_ratio = std::stod(std::string(tokens[15]));

      AuditResult row_res = audit_step_record(rec, eps);
      if (!row_res.passed) {
        result.passed = false;
        result.violations.insert(result.violations.end(),
                                 row_res.violations.begin(),
                                 row_res.violations.end());
      }
    } catch (const std::exception& ex) {
      result.passed = false;
      result.violations.push_back(InvariantViolation{
          .code = InvariantCode::INV_MetricRangeViolation,
          .req_id = static_cast<RequestId>(row_idx),
          .message = "Exception parsing CSV row: " + std::string(ex.what()),
      });
    }
    ++row_idx;
  }

  return result;
}

}  // namespace ton
