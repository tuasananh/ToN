#include <filesystem>
#include <fstream>
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

TEST_CASE("InvariantAuditor: Synthetic Graph Auditing & 0 False Positives",
          "[verification][auditor][synthetic]") {
  SECTION("Line graph: valid primary allocation without failure scenarios") {
    const std::vector<ton::NodeInfo> nodes{
        {.id = 0, .name = "N0", .lon_deg = 0.0, .lat_deg = 0.0},
        {.id = 1, .name = "N1", .lon_deg = 1.0, .lat_deg = 0.0},
        {.id = 2, .name = "N2", .lon_deg = 2.0, .lat_deg = 0.0},
    };
    const std::vector<ton::SpanInput> spans{
        {.name = "S01", .u = 0, .v = 1, .override_length_km = 10.0},
        {.name = "S12", .u = 1, .v = 2, .override_length_km = 10.0},
    };
    const ton::Graph line_graph("line3", nodes, spans);
    const auto scenarios =
        ton::ScenarioSet::enumerate(line_graph, ton::CutModel::Span,
                                    /*enforce_benchmark_assertions=*/false);
    ton::PathPool pool(line_graph, ton::DistanceMetric::HopCount, 0.10, 5);
    pool.prewarm_all_healthy();

    ton::NetworkState state(line_graph, scenarios,
                            ton::FailureRegime::Simultaneous, 16);

    // Empty state audit
    auto res_empty = ton::InvariantAuditor::audit_state(state, pool);
    REQUIRE(res_empty.passed);
    REQUIRE(res_empty.violations.empty());
    REQUIRE(res_empty.audited_admitted_requests == 0);

    // Commit a valid primary lightpath
    const auto routes = pool.find_healthy(0, 2);
    REQUIRE(!routes.empty());
    const ton::Request req{.id = 0, .src = 0, .dst = 2};
    const ton::Lightpath prim{.path_id = routes[0], .wavelength = 0};
    const ton::StagedAssignments staged(16, 64);
    state.commit(req, prim, staged, pool);

    auto res_prim = ton::InvariantAuditor::audit_state(state, pool);
    REQUIRE(res_prim.passed);
    REQUIRE(res_prim.violations.empty());
    REQUIRE(res_prim.audited_admitted_requests == 1);
    REQUIRE(res_prim.evaluated_metrics.h_prim_slots == 2);
    REQUIRE(res_prim.evaluated_metrics.h_back_slots == 0);
  }

  SECTION("Diamond mesh graph: Simultaneous and Sequential admitted states") {
    const std::vector<ton::NodeInfo> nodes{
        {.id = 0, .name = "N0", .lon_deg = 0.0, .lat_deg = 0.0},
        {.id = 1, .name = "N1", .lon_deg = 1.0, .lat_deg = 0.5},
        {.id = 2, .name = "N2", .lon_deg = 1.0, .lat_deg = -0.5},
        {.id = 3, .name = "N3", .lon_deg = 2.0, .lat_deg = 0.0},
    };
    const std::vector<ton::SpanInput> spans{
        {.name = "S01", .u = 0, .v = 1, .override_length_km = 10.0},
        {.name = "S02", .u = 0, .v = 2, .override_length_km = 10.0},
        {.name = "S12", .u = 1, .v = 2, .override_length_km = 10.0},
        {.name = "S13", .u = 1, .v = 3, .override_length_km = 10.0},
        {.name = "S23", .u = 2, .v = 3, .override_length_km = 10.0},
    };
    const ton::Graph diamond("diamond4", nodes, spans);
    const auto scenarios_dir =
        ton::ScenarioSet::enumerate(diamond, ton::CutModel::Directional,
                                    /*enforce_benchmark_assertions=*/false);
    REQUIRE(!scenarios_dir.sim_scenarios().empty());
    REQUIRE(!scenarios_dir.seq_scenarios().empty());

    const std::vector<ton::Request> requests = {
        {.id = 0, .src = 0, .dst = 3},
        {.id = 1, .src = 1, .dst = 2},
        {.id = 2, .src = 3, .dst = 0},
    };

    ton::PathPool pool(diamond, ton::DistanceMetric::HopCount, 0.50, 10);
    pool.prewarm_for_stream(requests, scenarios_dir);

    // Simultaneous SimRDF
    {
      ton::NetworkState sim_state(diamond, scenarios_dir,
                                  ton::FailureRegime::Simultaneous, 16);
      ton::SimOrchestrator<ton::DsaturSolver> orch(diamond, scenarios_dir, pool,
                                                   16);
      for (const auto& r : requests) {
        (void)orch.admit_request(r, sim_state);
      }
      REQUIRE(sim_state.num_admitted() > 0);

      auto res = ton::InvariantAuditor::audit_state(sim_state, pool);
      REQUIRE(res.passed);
      REQUIRE(res.violations.empty());
      REQUIRE(res.audited_failure_states ==
              scenarios_dir.sim_scenarios().size());
      REQUIRE(res.evaluated_metrics ==
              ton::SpectralTelemetry::evaluate(sim_state));
    }

    // Sequential SeqRDF
    {
      ton::NetworkState seq_state(diamond, scenarios_dir,
                                  ton::FailureRegime::Sequential, 16);
      ton::SeqOrchestrator<ton::DsaturSolver> orch(diamond, scenarios_dir, pool,
                                                   16);
      for (const auto& r : requests) {
        (void)orch.admit_request(r, seq_state);
      }
      REQUIRE(seq_state.num_admitted() > 0);

      auto res = ton::InvariantAuditor::audit_state(seq_state, pool);
      REQUIRE(res.passed);
      REQUIRE(res.violations.empty());
      REQUIRE(res.audited_failure_states ==
              scenarios_dir.active_stage1_units().size() +
                  scenarios_dir.seq_scenarios().size());
      REQUIRE(res.evaluated_metrics ==
              ton::SpectralTelemetry::evaluate(seq_state));
    }
  }

  SECTION("Cyclic chord graph: 5-node chorded ring under Span cuts") {
    const std::vector<ton::NodeInfo> nodes{
        {.id = 0, .name = "N0", .lon_deg = 0.0, .lat_deg = 1.0},
        {.id = 1, .name = "N1", .lon_deg = 1.0, .lat_deg = 0.8},
        {.id = 2, .name = "N2", .lon_deg = 0.8, .lat_deg = -0.5},
        {.id = 3, .name = "N3", .lon_deg = -0.8, .lat_deg = -0.5},
        {.id = 4, .name = "N4", .lon_deg = -1.0, .lat_deg = 0.8},
    };
    const std::vector<ton::SpanInput> spans{
        {.name = "S01", .u = 0, .v = 1, .override_length_km = 10.0},
        {.name = "S12", .u = 1, .v = 2, .override_length_km = 10.0},
        {.name = "S23", .u = 2, .v = 3, .override_length_km = 10.0},
        {.name = "S34", .u = 3, .v = 4, .override_length_km = 10.0},
        {.name = "S40", .u = 4, .v = 0, .override_length_km = 10.0},
        // Chords
        {.name = "S02", .u = 0, .v = 2, .override_length_km = 15.0},
        {.name = "S13", .u = 1, .v = 3, .override_length_km = 15.0},
        {.name = "S24", .u = 2, .v = 4, .override_length_km = 15.0},
    };
    const ton::Graph cyclic_mesh("cycle5_chords", nodes, spans);
    const auto scenarios_span =
        ton::ScenarioSet::enumerate(cyclic_mesh, ton::CutModel::Span,
                                    /*enforce_benchmark_assertions=*/false);
    REQUIRE(!scenarios_span.sim_scenarios().empty());

    const std::vector<ton::Request> requests = {
        {.id = 0, .src = 0, .dst = 3},
        {.id = 1, .src = 1, .dst = 4},
        {.id = 2, .src = 2, .dst = 0},
    };

    ton::PathPool pool(cyclic_mesh, ton::DistanceMetric::HopCount, 0.50, 10);
    pool.prewarm_for_stream(requests, scenarios_span);

    ton::NetworkState state(cyclic_mesh, scenarios_span,
                            ton::FailureRegime::Simultaneous, 16);
    ton::SimOrchestrator<ton::DsaturSolver> orch(cyclic_mesh, scenarios_span,
                                                 pool, 16);
    for (const auto& r : requests) {
      (void)orch.admit_request(r, state);
    }
    REQUIRE(state.num_admitted() > 0);

    auto res = ton::InvariantAuditor::audit_state(state, pool);
    REQUIRE(res.passed);
    REQUIRE(res.violations.empty());
  }
}

TEST_CASE("InvariantAuditor: SNDlib Nobel-EU & Germany50 Benchmark Invariance",
          "[verification][auditor][sndlib]") {
  SECTION("Nobel-EU Span cuts: SimRDF and SeqRDF zero false positives") {
    const auto nobel = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
    const auto nobel_span =
        ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
    const auto stream = ton::load_request_stream_csv(
        ton::test::repo_root() / "data" / "streams" /
            "nobel_eu_realistic_seed_01.csv",
        50);
    REQUIRE(stream.size() == 50);

    ton::PathPool pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
    pool.prewarm_for_stream(stream, nobel_span);

    // Simultaneous SimRDF
    {
      const auto run = ton::run_provisioning_algorithm(
          ton::AlgorithmId::SimRDF, nobel, nobel_span, pool, stream);
      REQUIRE(run.summary.total_granted > 0);

      const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
      REQUIRE(audit.passed);
      REQUIRE(audit.violations.empty());
      REQUIRE(audit.audited_admitted_requests == run.state.num_admitted());
      REQUIRE(audit.audited_failure_states ==
              nobel_span.sim_scenarios().size());
      REQUIRE(audit.evaluated_metrics ==
              ton::SpectralTelemetry::evaluate(run.state));
    }

    // Sequential SeqRDF
    {
      const auto run = ton::run_provisioning_algorithm(
          ton::AlgorithmId::SeqRDF, nobel, nobel_span, pool, stream);
      REQUIRE(run.summary.total_granted > 0);

      const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
      REQUIRE(audit.passed);
      REQUIRE(audit.violations.empty());
      REQUIRE(audit.audited_admitted_requests == run.state.num_admitted());
      REQUIRE(audit.audited_failure_states ==
              nobel_span.active_stage1_units().size() +
                  nobel_span.seq_scenarios().size());
      REQUIRE(audit.evaluated_metrics ==
              ton::SpectralTelemetry::evaluate(run.state));
    }
  }

  SECTION(
      "Nobel-EU Directional cuts: Greedy & NoReroute zero false positives") {
    const auto nobel = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
    const auto nobel_dir =
        ton::ScenarioSet::enumerate(nobel, ton::CutModel::Directional);
    const auto stream = ton::load_request_stream_csv(
        ton::test::repo_root() / "data" / "streams" /
            "nobel_eu_realistic_seed_01.csv",
        30);
    REQUIRE(stream.size() == 30);

    ton::PathPool pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
    pool.prewarm_for_stream(stream, nobel_dir);

    // GreedySimRDF
    {
      const auto run = ton::run_provisioning_algorithm(
          ton::AlgorithmId::GreedySimRDF, nobel, nobel_dir, pool, stream);
      REQUIRE(run.summary.total_granted > 0);
      const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
      REQUIRE(audit.passed);
      REQUIRE(audit.violations.empty());
    }

    // NoRerouteSeqRDF
    {
      const auto run = ton::run_provisioning_algorithm(
          ton::AlgorithmId::NoRerouteSeqRDF, nobel, nobel_dir, pool, stream);
      REQUIRE(run.summary.total_granted > 0);
      const auto audit = ton::InvariantAuditor::audit_state(run.state, pool);
      REQUIRE(audit.passed);
      REQUIRE(audit.violations.empty());
    }
  }
}

TEST_CASE("InvariantAuditor: Trajectory Record and CSV Metric Audit",
          "[verification][auditor][metrics]") {
  SECTION("Valid StepRecord metrics pass audit") {
    ton::StepRecord rec{
        .t = 0,
        .src = 1,
        .dst = 2,
        .admitted = true,
        .latency_us = 120.5,
        .k_adm = 1,
        .spectral =
            {
                .h_prim_slots = 10,
                .h_back_slots = 15,
                .h_pure_slots = 8,
                .h_stub_slots = 7,
                .u_prim_pct = 10.0,
                .u_back_pct = 15.0,
                .u_pure_pct = 8.0,
                .u_stub_pct = 7.0,
                .u_total_pct = 18.0,
                .overhead_ratio = 0.8,
            },
    };

    auto res = ton::InvariantAuditor::audit_step_record(rec);
    REQUIRE(res.passed);
    REQUIRE(res.violations.empty());
  }

  SECTION("Corrupted StepRecord metrics detected") {
    // Range violation: > 100%
    ton::StepRecord rec_invalid_range{
        .t = 1,
        .spectral =
            {
                .h_prim_slots = 10,
                .u_prim_pct = 105.0,
            },
    };
    auto res_range =
        ton::InvariantAuditor::audit_step_record(rec_invalid_range);
    REQUIRE_FALSE(res_range.passed);
    REQUIRE(
        res_range.has_violation(ton::InvariantCode::INV_MetricRangeViolation));

    // Consistency violation: u_total != u_prim + u_pure
    ton::StepRecord rec_inconsistent{
        .t = 2,
        .spectral =
            {
                .h_prim_slots = 10,
                .h_back_slots = 15,
                .h_pure_slots = 8,
                .h_stub_slots = 7,
                .u_prim_pct = 10.0,
                .u_back_pct = 15.0,
                .u_pure_pct = 8.0,
                .u_stub_pct = 7.0,
                .u_total_pct = 25.0,  // Expected 18.0
                .overhead_ratio = 0.8,
            },
    };
    auto res_incons =
        ton::InvariantAuditor::audit_step_record(rec_inconsistent);
    REQUIRE_FALSE(res_incons.passed);
    REQUIRE(
        res_incons.has_violation(ton::InvariantCode::INV_MetricConsistency));
  }

  SECTION("audit_trajectory_csv on written CSV") {
    const std::filesystem::path test_csv =
        ton::test::repo_root() / "build" / "test_traj.csv";
    std::ofstream ofs(test_csv);
    ton::SpectralTelemetry::write_csv_header(ofs);

    ton::StepRecord rec1{
        .t = 0,
        .src = 0,
        .dst = 1,
        .admitted = true,
        .latency_us = 50.0,
        .k_adm = 1,
        .spectral =
            {
                .h_prim_slots = 5,
                .h_back_slots = 5,
                .h_pure_slots = 3,
                .h_stub_slots = 2,
                .u_prim_pct = 5.0,
                .u_back_pct = 5.0,
                .u_pure_pct = 3.0,
                .u_stub_pct = 2.0,
                .u_total_pct = 8.0,
                .overhead_ratio = 0.6,
            },
    };
    ton::SpectralTelemetry::write_csv_row(ofs, rec1);
    ofs.close();

    auto audit_csv = ton::InvariantAuditor::audit_trajectory_csv(test_csv);
    REQUIRE(audit_csv.passed);
    REQUIRE(audit_csv.violations.empty());

    std::filesystem::remove(test_csv);
  }

  SECTION("audit_trajectory_csv negative tests for corrupted files") {
    // 1. Nonexistent file
    {
      const auto audit = ton::InvariantAuditor::audit_trajectory_csv(
          ton::test::repo_root() / "build" / "nonexistent_file.csv");
      REQUIRE_FALSE(audit.passed);
      REQUIRE(!audit.violations.empty());
    }

    // 2. Empty file
    {
      const std::filesystem::path empty_csv =
          ton::test::repo_root() / "build" / "test_empty.csv";
      std::ofstream ofs(empty_csv);
      ofs.close();

      const auto audit = ton::InvariantAuditor::audit_trajectory_csv(empty_csv);
      REQUIRE_FALSE(audit.passed);
      REQUIRE(!audit.violations.empty());
      REQUIRE(audit.violations[0].message.find("Empty CSV") !=
              std::string::npos);

      std::filesystem::remove(empty_csv);
    }

    // 3. Corrupted header
    {
      const std::filesystem::path bad_header_csv =
          ton::test::repo_root() / "build" / "test_bad_header.csv";
      std::ofstream ofs(bad_header_csv);
      ofs << "foo,bar,baz,col4,col5\n";
      ofs << "0,0,1,1,50.0,1,5,5,3,2,5.0,5.0,3.0,2.0,8.0,0.6\n";
      ofs.close();

      const auto audit =
          ton::InvariantAuditor::audit_trajectory_csv(bad_header_csv);
      REQUIRE_FALSE(audit.passed);
      REQUIRE(!audit.violations.empty());
      REQUIRE(audit.violations[0].message.find("header") != std::string::npos);

      std::filesystem::remove(bad_header_csv);
    }

    // 4. Malformed / truncated row (< 16 tokens)
    {
      const std::filesystem::path truncated_row_csv =
          ton::test::repo_root() / "build" / "test_truncated_row.csv";
      std::ofstream ofs(truncated_row_csv);
      ton::SpectralTelemetry::write_csv_header(ofs);
      ofs << "0,0,1,1,50.0\n";  // only 5 columns
      ofs.close();

      const auto audit =
          ton::InvariantAuditor::audit_trajectory_csv(truncated_row_csv);
      REQUIRE_FALSE(audit.passed);
      REQUIRE(!audit.violations.empty());
      REQUIRE(audit.violations[0].message.find("Malformed row") !=
              std::string::npos);

      std::filesystem::remove(truncated_row_csv);
    }
  }
}
