#include <chrono>
#include <vector>

#include "test_helpers.hpp"
#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/orchestration/provisioning_harness.hpp"
#include "ton/orchestration/seq_orchestrator.hpp"
#include "ton/orchestration/sim_orchestrator.hpp"
#include "ton/solvers/dsatur_solver.hpp"
#include "ton/solvers/greedy_solver.hpp"
#include "ton/solvers/no_reroute_solver.hpp"
#include "ton/verification/invariant_auditor.hpp"

namespace {

template <typename Orchestrator>
void run_and_audit_single_pass(ton::AlgorithmId alg, const ton::Graph& graph,
                               const ton::ScenarioSet& scenarios,
                               const ton::PathPool& pool,
                               std::span<const ton::Request> stream,
                               std::span<const size_t> audit_points,
                               bool check_repeatability) {
  const auto max_req = static_cast<uint16_t>(stream.size() + 16u);
  ton::NetworkState state(graph, scenarios, ton::algorithm_regime(alg),
                          max_req);
  Orchestrator orch(graph, scenarios, pool, max_req);
  std::vector<ton::StepRecord> records(stream.size());

  size_t audit_idx = 0;
  bool monitor_armed = false;

  for (size_t i = 0; i < stream.size(); ++i) {
    if (!monitor_armed && !ton::test::is_asan_active()) {
      ton::test::arm_zero_alloc_monitor();
      monitor_armed = true;
    }

    const ton::Request& req = stream[i];
    const auto t0 = std::chrono::steady_clock::now();
    const ton::AdmissionResult res = orch.admit_request(req, state);
    const auto t1 = std::chrono::steady_clock::now();
    const double latency_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count();

    records[i] = ton::StepRecord{
        .t = req.id,
        .src = req.src,
        .dst = req.dst,
        .admitted = res.admitted,
        .latency_us = latency_us,
        .k_adm = state.num_admitted(),
        .spectral = ton::SpectralTelemetry::evaluate(state),
    };

    const bool is_checkpoint =
        (audit_idx < audit_points.size() && i == audit_points[audit_idx]) ||
        (i + 1 == stream.size());

    if (is_checkpoint) {
      if (monitor_armed && !ton::test::is_asan_active()) {
        ton::test::disarm_zero_alloc_monitor();
        monitor_armed = false;
        REQUIRE(ton::test::tracked_allocations() == 0);
      }

      auto audit = ton::InvariantAuditor::audit_state(state, pool);
      REQUIRE(audit.passed);
      REQUIRE(audit.violations.empty());
      REQUIRE(audit.evaluated_metrics ==
              ton::SpectralTelemetry::evaluate(state));

      if (audit_idx < audit_points.size() && i == audit_points[audit_idx]) {
        ++audit_idx;
      }
    }
  }

  if (monitor_armed && !ton::test::is_asan_active()) {
    ton::test::disarm_zero_alloc_monitor();
    REQUIRE(ton::test::tracked_allocations() == 0);
  }

  // Audit all step trajectory records against metric bounds
  for (const auto& rec : records) {
    auto rec_audit = ton::InvariantAuditor::audit_step_record(rec);
    REQUIRE(rec_audit.passed);
  }

  if (check_repeatability) {
    state.reset();
    orch.reset();
    std::vector<ton::StepRecord> records_replay(stream.size());

    for (size_t i = 0; i < stream.size(); ++i) {
      const ton::Request& req = stream[i];
      const auto t0 = std::chrono::steady_clock::now();
      const ton::AdmissionResult res = orch.admit_request(req, state);
      const auto t1 = std::chrono::steady_clock::now();
      const double latency_us =
          std::chrono::duration<double, std::micro>(t1 - t0).count();

      records_replay[i] = ton::StepRecord{
          .t = req.id,
          .src = req.src,
          .dst = req.dst,
          .admitted = res.admitted,
          .latency_us = latency_us,
          .k_adm = state.num_admitted(),
          .spectral = ton::SpectralTelemetry::evaluate(state),
      };

      REQUIRE(records[i].admitted == records_replay[i].admitted);
      REQUIRE(records[i].k_adm == records_replay[i].k_adm);
      REQUIRE(records[i].spectral == records_replay[i].spectral);
    }

    auto replay_audit = ton::InvariantAuditor::audit_state(state, pool);
    REQUIRE(replay_audit.passed);
  }
}

void execute_phase4_audit(ton::AlgorithmId alg, const ton::Graph& graph,
                          const ton::ScenarioSet& scenarios,
                          const ton::PathPool& pool,
                          std::span<const ton::Request> stream,
                          std::span<const size_t> audit_points,
                          bool check_repeatability = true) {
  switch (alg) {
    case ton::AlgorithmId::SimRDF:
      run_and_audit_single_pass<ton::SimRdfOrchestrator>(
          alg, graph, scenarios, pool, stream, audit_points,
          check_repeatability);
      break;
    case ton::AlgorithmId::SeqRDF:
      run_and_audit_single_pass<ton::SeqRdfOrchestrator>(
          alg, graph, scenarios, pool, stream, audit_points,
          check_repeatability);
      break;
    case ton::AlgorithmId::GreedySimRDF:
      run_and_audit_single_pass<ton::GreedySimRdfOrchestrator>(
          alg, graph, scenarios, pool, stream, audit_points,
          check_repeatability);
      break;
    case ton::AlgorithmId::GreedySeqRDF:
      run_and_audit_single_pass<ton::GreedySeqRdfOrchestrator>(
          alg, graph, scenarios, pool, stream, audit_points,
          check_repeatability);
      break;
    case ton::AlgorithmId::NoRerouteSimRDF:
      run_and_audit_single_pass<ton::NoRerouteSimRdfOrchestrator>(
          alg, graph, scenarios, pool, stream, audit_points,
          check_repeatability);
      break;
    case ton::AlgorithmId::NoRerouteSeqRDF:
      run_and_audit_single_pass<ton::NoRerouteSeqRdfOrchestrator>(
          alg, graph, scenarios, pool, stream, audit_points,
          check_repeatability);
      break;
  }
}

}  // namespace

TEST_CASE(
    "Phase 4 Gate: Multi-Seed Periodic & Full-Stream Invariant Verification",
    "[verification][gate][phase4]") {
  const auto nobel = ton::Graph::from_sndlib_xml(
      ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
  const auto nobel_span =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
  const auto nobel_dir =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Directional);

  SECTION(
      "Nobel-EU: Seeds 01, 02, 03 Span Cuts Periodic Auditing (t=100, 250, "
      "500, 750, 999)") {
    const std::vector<size_t> audit_pts = {100, 250, 500, 750, 999};

    for (const char* seed_file :
         {"nobel_eu_realistic_seed_01.csv", "nobel_eu_realistic_seed_02.csv",
          "nobel_eu_realistic_seed_03.csv"}) {
      const auto stream = ton::load_request_stream_csv(
          ton::test::repo_root() / "data" / "streams" / seed_file, 1000);
      REQUIRE(stream.size() == 1000);

      ton::PathPool pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
      pool.prewarm_for_stream(stream, nobel_span);

      for (const ton::AlgorithmId alg : ton::ALL_ALGORITHMS) {
        execute_phase4_audit(
            alg, nobel, nobel_span, pool, stream, audit_pts,
            /*check_repeatability=*/(alg == ton::AlgorithmId::SimRDF));
      }
    }
  }

  SECTION("Nobel-EU: Seeds 01, 02, 03 Directional Cuts Periodic Auditing") {
    const std::vector<size_t> audit_pts = {100, 250, 500};

    for (const char* seed_file :
         {"nobel_eu_realistic_seed_01.csv", "nobel_eu_realistic_seed_02.csv",
          "nobel_eu_realistic_seed_03.csv"}) {
      const auto stream = ton::load_request_stream_csv(
          ton::test::repo_root() / "data" / "streams" / seed_file, 500);
      REQUIRE(stream.size() == 500);

      ton::PathPool pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
      pool.prewarm_for_stream(stream, nobel_dir);

      for (const ton::AlgorithmId alg : ton::ALL_ALGORITHMS) {
        execute_phase4_audit(
            alg, nobel, nobel_dir, pool, stream, audit_pts,
            /*check_repeatability=*/(alg == ton::AlgorithmId::SeqRDF));
      }
    }
  }

  SECTION("Germany50: Seeds 01, 02, 03 Scale Verification Gate") {
    const auto germany = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "germany50.xml");
    const auto germany_span =
        ton::ScenarioSet::enumerate(germany, ton::CutModel::Span);

#if defined(NDEBUG)
    constexpr size_t kGermanyReqs = 1000;
    const std::vector<size_t> audit_pts = {100, 250, 500, 750, 999};
#else
    constexpr size_t kGermanyReqs = 200;
    const std::vector<size_t> audit_pts = {50, 100, 150, 199};
#endif

    for (const char* seed_file :
         {"germany50_1d1m_seed_01.csv", "germany50_1d1m_seed_02.csv",
          "germany50_1d1m_seed_03.csv"}) {
      const auto stream = ton::load_request_stream_csv(
          ton::test::repo_root() / "data" / "streams" / seed_file,
          kGermanyReqs);
      REQUIRE(stream.size() == kGermanyReqs);

      ton::PathPool pool(germany, ton::DistanceMetric::GeographicKm, 0.10, 10);
      pool.prewarm_for_stream(stream, germany_span);

      for (const ton::AlgorithmId alg : ton::ALL_ALGORITHMS) {
        execute_phase4_audit(
            alg, germany, germany_span, pool, stream, audit_pts,
            /*check_repeatability=*/(alg == ton::AlgorithmId::SimRDF));
      }
    }
  }

  SECTION(
      "Germany50: Seeds 01, 02, 03 Directional Cuts Scale Verification Gate") {
    const auto germany = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "germany50.xml");
    const auto germany_dir =
        ton::ScenarioSet::enumerate(germany, ton::CutModel::Directional);

#if defined(NDEBUG)
    constexpr size_t kGermanyReqs = 1000;
    const std::vector<size_t> audit_pts = {250, 500, 750, 999};
#else
    constexpr size_t kGermanyReqs = 200;
    const std::vector<size_t> audit_pts = {50, 100, 150, 199};
#endif

    for (const char* seed_file :
         {"germany50_1d1m_seed_01.csv", "germany50_1d1m_seed_02.csv",
          "germany50_1d1m_seed_03.csv"}) {
      const auto stream = ton::load_request_stream_csv(
          ton::test::repo_root() / "data" / "streams" / seed_file,
          kGermanyReqs);
      REQUIRE(stream.size() == kGermanyReqs);

      ton::PathPool pool(germany, ton::DistanceMetric::GeographicKm, 0.10, 10);
      pool.prewarm_for_stream(stream, germany_dir);

      for (const ton::AlgorithmId alg : ton::ALL_ALGORITHMS) {
        execute_phase4_audit(
            alg, germany, germany_dir, pool, stream, audit_pts,
            /*check_repeatability=*/(alg == ton::AlgorithmId::SimRDF));
      }
    }
  }
}
