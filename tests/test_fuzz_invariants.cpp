#include <algorithm>
#include <vector>

#include "test_helpers.hpp"
#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/orchestration/provisioning_harness.hpp"
#include "ton/orchestration/seq_orchestrator.hpp"
#include "ton/orchestration/sim_orchestrator.hpp"
#include "ton/solvers/dsatur_solver.hpp"
#include "ton/verification/invariant_auditor.hpp"

namespace ton::test {

class InvariantFuzzInjector {
 public:
  // 1. INV-1 Primary Clash
  static void inject_primary_clash(NetworkState& state, RequestId r1,
                                   RequestId r2, ArcId a, uint8_t w) {
    state.primary_owner_[static_cast<size_t>(a) * NUM_WAVELENGTHS + w] = r1;
    // Set r2 primary lightpath to share (a, w)
    state.primary_lightpaths_[r2].wavelength = w;
  }

  // 2. INV-1 Backup Clash
  static void inject_backup_clash(NetworkState& state, ScenarioId f,
                                  const BackupAssignment& colliding_ba) {
    auto& tbl = state.sim_table_;
    const size_t offset = static_cast<size_t>(f) * tbl.per_state_cap;
    tbl.assignments[offset + tbl.counts[f]++] = colliding_ba;
  }

  // 3. INV-1 Illegal Collision with Intact Primary
  static void inject_illegal_collision(NetworkState& state, ScenarioId f,
                                       RequestId backup_req, PathId path_id,
                                       uint8_t intact_w) {
    auto& tbl = state.sim_table_;
    const size_t offset = static_cast<size_t>(f) * tbl.per_state_cap;
    for (uint16_t i = 0; i < tbl.counts[f]; ++i) {
      if (tbl.assignments[offset + i].req_id == backup_req) {
        tbl.assignments[offset + i].path_id = path_id;
        tbl.assignments[offset + i].wavelength = intact_w;
        return;
      }
    }
  }

  // 4. INV-2 Wavelength Discontinuity (invalid wavelength or hop-level
  // mismatch)
  static void inject_primary_wavelength_out_of_range(NetworkState& state,
                                                     RequestId r,
                                                     uint8_t invalid_w) {
    state.primary_lightpaths_[r].wavelength = invalid_w;
  }

  static void inject_primary_hop_wavelength_desync(NetworkState& state,
                                                   RequestId r, ArcId a,
                                                   uint8_t desync_w) {
    state.primary_owner_[static_cast<size_t>(a) * NUM_WAVELENGTHS + desync_w] =
        r;
  }

  // 5. INV-3 False Backup
  static void inject_false_backup(NetworkState& state, ScenarioId f,
                                  RequestId intact_req, PathId path_id,
                                  uint8_t w) {
    auto& tbl = state.sim_table_;
    const size_t offset = static_cast<size_t>(f) * tbl.per_state_cap;
    tbl.assignments[offset + tbl.counts[f]++] = BackupAssignment{
        .path_id = path_id,
        .req_id = intact_req,
        .wavelength = w,
    };
  }

  // 6. INV-3 Missed Backup
  static void inject_missed_backup(NetworkState& state, ScenarioId f,
                                   RequestId severed_req) {
    auto& tbl = state.sim_table_;
    const size_t offset = static_cast<size_t>(f) * tbl.per_state_cap;
    for (uint16_t i = 0; i < tbl.counts[f]; ++i) {
      if (tbl.assignments[offset + i].req_id == severed_req) {
        for (uint16_t j = i + 1; j < tbl.counts[f]; ++j) {
          tbl.assignments[offset + j - 1] = tbl.assignments[offset + j];
        }
        --tbl.counts[f];
        return;
      }
    }
  }

  // 7. INV-4 Cut Arcs Traversed
  static void inject_cut_arc_traversed(NetworkState& state, ScenarioId f,
                                       RequestId severed_req,
                                       PathId cut_traversing_path) {
    auto& tbl = state.sim_table_;
    const size_t offset = static_cast<size_t>(f) * tbl.per_state_cap;
    for (uint16_t i = 0; i < tbl.counts[f]; ++i) {
      if (tbl.assignments[offset + i].req_id == severed_req) {
        tbl.assignments[offset + i].path_id = cut_traversing_path;
        return;
      }
    }
  }

  // 8. INV-4 Invalid Route (out-of-range path ID)
  static void inject_invalid_route(NetworkState& state, ScenarioId f,
                                   RequestId severed_req, PathId invalid_path) {
    auto& tbl = state.sim_table_;
    const size_t offset = static_cast<size_t>(f) * tbl.per_state_cap;
    for (uint16_t i = 0; i < tbl.counts[f]; ++i) {
      if (tbl.assignments[offset + i].req_id == severed_req) {
        tbl.assignments[offset + i].path_id = invalid_path;
        return;
      }
    }
  }

  static void inject_seq_s1_invalid_route(NetworkState& state, FailureUnitId u,
                                          RequestId severed_req,
                                          PathId invalid_path) {
    auto& tbl = state.s1_table_;
    const size_t offset = static_cast<size_t>(u) * tbl.per_state_cap;
    for (uint16_t i = 0; i < tbl.counts[u]; ++i) {
      if (tbl.assignments[offset + i].req_id == severed_req) {
        tbl.assignments[offset + i].path_id = invalid_path;
        return;
      }
    }
  }

  static void inject_seq_s2_invalid_route(NetworkState& state, ScenarioId f,
                                          RequestId severed_req,
                                          PathId invalid_path) {
    auto& tbl = state.s2_table_;
    const size_t offset = static_cast<size_t>(f) * tbl.per_state_cap;
    for (uint16_t i = 0; i < tbl.counts[f]; ++i) {
      if (tbl.assignments[offset + i].req_id == severed_req) {
        tbl.assignments[offset + i].path_id = invalid_path;
        return;
      }
    }
  }

  // 9. INV-5 Sequential Desync (allocations in inactive Stage-1 unit)
  static void inject_sequential_desync(NetworkState& state,
                                       FailureUnitId inactive_u,
                                       const BackupAssignment& ba) {
    auto& tbl = state.s1_table_;
    const size_t offset = static_cast<size_t>(inactive_u) * tbl.per_state_cap;
    tbl.assignments[offset + tbl.counts[inactive_u]++] = ba;
  }

  // 10. INV-5 Broken Sequential Lock
  static void inject_broken_sequential_lock(NetworkState& state, ScenarioId f,
                                            RequestId r, uint8_t altered_w) {
    auto& tbl = state.s2_table_;
    const size_t offset = static_cast<size_t>(f) * tbl.per_state_cap;
    for (uint16_t i = 0; i < tbl.counts[f]; ++i) {
      if (tbl.assignments[offset + i].req_id == r) {
        tbl.assignments[offset + i].wavelength = altered_w;
        return;
      }
    }
  }

  // 11. Spectral Refcount Desync
  static void inject_spectral_refcount_desync(NetworkState& state, ArcId a,
                                              uint8_t w, int16_t delta) {
    state.backup_slot_refcount_[static_cast<size_t>(a) * NUM_WAVELENGTHS + w] +=
        static_cast<uint16_t>(delta);
  }

  // 12. Spectral Active Mask Desync
  static void inject_spectral_active_mask_desync(NetworkState& state, ArcId a,
                                                 uint8_t w) {
    state.backup_active_mask_[a] ^= wavelength_bit(w);
  }
};

}  // namespace ton::test

TEST_CASE("Adversarial Invariant Fuzz Harness: 100% Detection Sensitivity",
          "[verification][fuzz][adversarial]") {
  const auto nobel = ton::Graph::from_sndlib_xml(
      ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
  const auto nobel_span =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
  const auto stream =
      ton::load_request_stream_csv(ton::test::repo_root() / "data" / "streams" /
                                       "nobel_eu_realistic_seed_01.csv",
                                   40);
  REQUIRE(stream.size() == 40);

  ton::PathPool pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
  pool.prewarm_for_stream(stream, nobel_span);

  SECTION("1. Injected INV-1 Primary Clash is detected with 100% sensitivity") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted >= 2);
    REQUIRE(ton::InvariantAuditor::audit_state(run.state, pool).passed);

    // Find two admitted requests sharing a physical arc
    ton::RequestId ra = ton::INVALID_REQUEST;
    ton::RequestId rb = ton::INVALID_REQUEST;
    ton::ArcId shared_a = ton::INVALID_ARC;

    for (size_t i = 0; i < run.state.num_admitted(); ++i) {
      const ton::RequestId r1 = run.state.admitted_ids()[i];
      const auto arcs1 =
          pool.path_arcs(run.state.primary_lightpath(r1).path_id);
      for (size_t j = i + 1; j < run.state.num_admitted(); ++j) {
        const ton::RequestId r2 = run.state.admitted_ids()[j];
        const auto arcs2 =
            pool.path_arcs(run.state.primary_lightpath(r2).path_id);
        for (const ton::ArcId a1 : arcs1) {
          for (const ton::ArcId a2 : arcs2) {
            if (a1 == a2) {
              ra = r1;
              rb = r2;
              shared_a = a1;
              break;
            }
          }
          if (shared_a != ton::INVALID_ARC) {
            break;
          }
        }
        if (shared_a != ton::INVALID_ARC) {
          break;
        }
      }
      if (shared_a != ton::INVALID_ARC) {
        break;
      }
    }
    REQUIRE(shared_a != ton::INVALID_ARC);

    const auto pa = run.state.primary_lightpath(ra);
    ton::test::InvariantFuzzInjector::inject_primary_clash(
        run.state, ra, rb, shared_a, pa.wavelength);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(audit.has_violation(ton::InvariantCode::INV1_PrimaryClash));
  }

  SECTION("2. Injected INV-1 Backup Clash is detected with 100% sensitivity") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    // Find scenario with at least 1 backup
    ton::ScenarioId target_f = ton::INVALID_SCENARIO;
    for (const auto& sc : nobel_span.sim_scenarios()) {
      if (run.state.sim_backups(sc.id).size() >= 1) {
        target_f = sc.id;
        break;
      }
    }
    REQUIRE(target_f != ton::INVALID_SCENARIO);

    const auto existing_ba = run.state.sim_backups(target_f)[0];
    // Inject duplicate backup on same route and wavelength
    ton::test::InvariantFuzzInjector::inject_backup_clash(run.state, target_f,
                                                          existing_ba);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(audit.has_violation(ton::InvariantCode::INV1_BackupClash));
  }

  SECTION(
      "3. Injected INV-1 Illegal Collision with Intact Primary is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted >= 2);

    ton::ScenarioId target_f = ton::INVALID_SCENARIO;
    ton::RequestId severed_r = ton::INVALID_REQUEST;
    ton::RequestId intact_r = ton::INVALID_REQUEST;

    for (const auto& sc : nobel_span.sim_scenarios()) {
      if (run.state.sim_backups(sc.id).size() >= 1) {
        const auto backups = run.state.sim_backups(sc.id);
        severed_r = backups[0].req_id;
        for (const ton::RequestId r : run.state.admitted_ids()) {
          const auto prim = run.state.primary_lightpath(r);
          if (!pool.path_intersects(prim.path_id, sc.cut_arcs)) {
            intact_r = r;
            target_f = sc.id;
            break;
          }
        }
        if (target_f != ton::INVALID_SCENARIO) {
          break;
        }
      }
    }

    REQUIRE(target_f != ton::INVALID_SCENARIO);
    const auto intact_prim = run.state.primary_lightpath(intact_r);

    // Corrupt severed_r backup to use intact_r primary path and wavelength
    ton::test::InvariantFuzzInjector::inject_illegal_collision(
        run.state, target_f, severed_r, intact_prim.path_id,
        intact_prim.wavelength);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(audit.has_violation(ton::InvariantCode::INV1_IllegalCollision));
  }

  SECTION("4. Injected INV-2 Wavelength Discontinuity is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    const ton::RequestId r = run.state.admitted_ids()[0];
    // A. Out of range wavelength
    {
      auto state_copy = run.state;
      ton::test::InvariantFuzzInjector::inject_primary_wavelength_out_of_range(
          state_copy, r, 32);
      const auto audit = ton::InvariantAuditor::audit_state(state_copy, pool);
      REQUIRE_FALSE(audit.passed);
      REQUIRE(audit.has_violation(
          ton::InvariantCode::INV2_WavelengthDiscontinuity));
    }

    // B. Hop-level wavelength desync
    {
      auto state_copy = run.state;
      const auto lp = state_copy.primary_lightpath(r);
      const auto arcs = pool.path_arcs(lp.path_id);
      REQUIRE(!arcs.empty());
      ton::test::InvariantFuzzInjector::inject_primary_hop_wavelength_desync(
          state_copy, r, arcs[0],
          static_cast<uint8_t>((lp.wavelength + 1) % 32));
      const auto audit = ton::InvariantAuditor::audit_state(state_copy, pool);
      REQUIRE_FALSE(audit.passed);
      REQUIRE(audit.has_violation(
          ton::InvariantCode::INV2_WavelengthDiscontinuity));
    }
  }

  SECTION("5. Injected INV-3 False Backup is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    ton::ScenarioId target_f = ton::INVALID_SCENARIO;
    ton::RequestId intact_r = ton::INVALID_REQUEST;

    for (const auto& sc : nobel_span.sim_scenarios()) {
      for (const ton::RequestId r : run.state.admitted_ids()) {
        const auto prim = run.state.primary_lightpath(r);
        if (!pool.path_intersects(prim.path_id, sc.cut_arcs)) {
          intact_r = r;
          target_f = sc.id;
          break;
        }
      }
      if (target_f != ton::INVALID_SCENARIO) {
        break;
      }
    }
    REQUIRE(target_f != ton::INVALID_SCENARIO);

    const auto prim = run.state.primary_lightpath(intact_r);
    ton::test::InvariantFuzzInjector::inject_false_backup(
        run.state, target_f, intact_r, prim.path_id, prim.wavelength);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(audit.has_violation(ton::InvariantCode::INV3_FalseBackup));
  }

  SECTION("6. Injected INV-3 Missed Backup is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    ton::ScenarioId target_f = ton::INVALID_SCENARIO;
    ton::RequestId severed_r = ton::INVALID_REQUEST;

    for (const auto& sc : nobel_span.sim_scenarios()) {
      if (!run.state.sim_backups(sc.id).empty()) {
        target_f = sc.id;
        severed_r = run.state.sim_backups(sc.id)[0].req_id;
        break;
      }
    }
    REQUIRE(target_f != ton::INVALID_SCENARIO);

    ton::test::InvariantFuzzInjector::inject_missed_backup(run.state, target_f,
                                                           severed_r);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(audit.has_violation(ton::InvariantCode::INV3_MissedBackup));
  }

  SECTION("7. Injected INV-4 Cut Arcs Traversed is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    ton::ScenarioId target_f = ton::INVALID_SCENARIO;
    ton::RequestId severed_r = ton::INVALID_REQUEST;

    for (const auto& sc : nobel_span.sim_scenarios()) {
      if (!run.state.sim_backups(sc.id).empty()) {
        target_f = sc.id;
        severed_r = run.state.sim_backups(sc.id)[0].req_id;
        break;
      }
    }
    REQUIRE(target_f != ton::INVALID_SCENARIO);

    // Primary route of severed_r intersects cut_arcs by definition!
    const auto severed_prim = run.state.primary_lightpath(severed_r);
    ton::test::InvariantFuzzInjector::inject_cut_arc_traversed(
        run.state, target_f, severed_r, severed_prim.path_id);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(audit.has_violation(ton::InvariantCode::INV4_CutArcTraversed));
  }

  SECTION("8. Injected INV-4 Invalid Route is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    ton::ScenarioId target_f = ton::INVALID_SCENARIO;
    ton::RequestId severed_r = ton::INVALID_REQUEST;

    for (const auto& sc : nobel_span.sim_scenarios()) {
      if (!run.state.sim_backups(sc.id).empty()) {
        target_f = sc.id;
        severed_r = run.state.sim_backups(sc.id)[0].req_id;
        break;
      }
    }
    REQUIRE(target_f != ton::INVALID_SCENARIO);

    ton::test::InvariantFuzzInjector::inject_invalid_route(
        run.state, target_f, severed_r, ton::INVALID_PATH);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(audit.has_violation(ton::InvariantCode::INV4_RouteAdmissibility));

    // Sequential Stage 1 and Stage 2 invalid route tests
    {
      auto seq_run = ton::run_provisioning_algorithm(
          ton::AlgorithmId::SeqRDF, nobel, nobel_span, pool, stream);
      REQUIRE(seq_run.summary.total_granted > 0);

      ton::FailureUnitId target_u = ton::INVALID_FAILURE_UNIT;
      ton::RequestId s1_r = ton::INVALID_REQUEST;
      for (ton::FailureUnitId u = 0; u < nobel_span.num_units(); ++u) {
        if (!seq_run.state.seq_stage1_backups(u).empty()) {
          target_u = u;
          s1_r = seq_run.state.seq_stage1_backups(u)[0].req_id;
          break;
        }
      }
      if (target_u != ton::INVALID_FAILURE_UNIT) {
        ton::test::InvariantFuzzInjector::inject_seq_s1_invalid_route(
            seq_run.state, target_u, s1_r, ton::INVALID_PATH);
        const auto seq_audit =
            ton::InvariantAuditor::audit_state(seq_run.state, pool);
        REQUIRE_FALSE(seq_audit.passed);
        REQUIRE(seq_audit.has_violation(
            ton::InvariantCode::INV4_RouteAdmissibility));
      }
    }
  }

  SECTION("9. Injected INV-5 Sequential Desync (Inactive Unit) is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SeqRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    // Find any inactive unit in nobel_span (or unit with index num_units - 1 if
    // inactive)
    ton::FailureUnitId inactive_u = ton::INVALID_FAILURE_UNIT;
    for (ton::FailureUnitId u = 0; u < nobel_span.num_units(); ++u) {
      if (!nobel_span.is_active_stage1_unit(u)) {
        inactive_u = u;
        break;
      }
    }

    if (inactive_u != ton::INVALID_FAILURE_UNIT) {
      const ton::BackupAssignment fake_ba{
          .path_id = 0,
          .req_id = run.state.admitted_ids()[0],
          .wavelength = 0,
      };
      ton::test::InvariantFuzzInjector::inject_sequential_desync(
          run.state, inactive_u, fake_ba);

      const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
      REQUIRE_FALSE(audit.passed);
      REQUIRE(audit.has_violation(ton::InvariantCode::INV5_SequentialDesync));
    }
  }

  SECTION("10. Injected INV-5 Broken Sequential Lock is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SeqRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    // Find a sequential scenario where a Stage-1 backup survived Stage 2
    ton::ScenarioId target_f = ton::INVALID_SCENARIO;
    ton::RequestId locked_r = ton::INVALID_REQUEST;
    uint8_t original_w = 0;

    for (const auto& sc : nobel_span.seq_scenarios()) {
      const auto s1_backups = run.state.seq_stage1_backups(sc.x1);
      const auto s2_backups = run.state.seq_stage2_backups(sc.id);

      for (const auto& ba1 : s1_backups) {
        if (!pool.path_intersects(ba1.path_id, sc.stage2_cut)) {
          // Survived Stage 2! Must be present in s2_backups
          for (const auto& ba2 : s2_backups) {
            if (ba2.req_id == ba1.req_id) {
              target_f = sc.id;
              locked_r = ba1.req_id;
              original_w = ba1.wavelength;
              break;
            }
          }
        }
        if (target_f != ton::INVALID_SCENARIO) {
          break;
        }
      }
      if (target_f != ton::INVALID_SCENARIO) {
        break;
      }
    }

    REQUIRE(target_f != ton::INVALID_SCENARIO);

    // Break the lock by changing wavelength in Stage 2
    const uint8_t altered_w = static_cast<uint8_t>((original_w + 1) % 32);
    ton::test::InvariantFuzzInjector::inject_broken_sequential_lock(
        run.state, target_f, locked_r, altered_w);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(audit.has_violation(ton::InvariantCode::INV5_BrokenSequentialLock));
  }

  SECTION("11. Injected Spectral Refcount Desync is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    ton::test::InvariantFuzzInjector::inject_spectral_refcount_desync(run.state,
                                                                      0, 0, 1);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(
        audit.has_violation(ton::InvariantCode::INV_SpectralRefcountDesync));
  }

  SECTION("12. Injected Spectral Active Mask Desync is detected") {
    auto run = ton::run_provisioning_algorithm(ton::AlgorithmId::SimRDF, nobel,
                                               nobel_span, pool, stream);
    REQUIRE(run.summary.total_granted > 0);

    ton::test::InvariantFuzzInjector::inject_spectral_active_mask_desync(
        run.state, 0, 0);

    const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
    REQUIRE_FALSE(audit.passed);
    REQUIRE(
        audit.has_violation(ton::InvariantCode::INV_SpectralActiveMaskDesync));
  }
}
