#include <vector>

#include "test_helpers.hpp"
#include "ton/orchestration/provisioning_harness.hpp"

TEST_CASE("Phase 3 Gate: 6-Algorithm Harness & Zero-Allocation Online Loop",
          "[orchestration][gate]") {
  const auto nobel = ton::Graph::from_sndlib_xml(
      ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
  const auto nobel_span =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
  const auto n_stream =
      ton::load_request_stream_csv(ton::test::repo_root() / "data" / "streams" /
                                       "nobel_eu_realistic_seed_01.csv",
                                   1000);
  REQUIRE(n_stream.size() == 1000);

  ton::PathPool n_pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
  n_pool.prewarm_for_stream(n_stream, nobel_span);

  SECTION("Nobel-EU full T=1000 stream completion across all 6 algorithms") {
    for (const ton::AlgorithmId alg : ton::ALL_ALGORITHMS) {
      ton::OnlineLoopHooks hooks{};
      if (!ton::test::is_asan_active()) {
        hooks.on_before_loop = ton::test::arm_zero_alloc_monitor;
        hooks.on_after_loop = ton::test::disarm_zero_alloc_monitor;
      }

      const ton::ProvisioningRunResult result = ton::run_provisioning_algorithm(
          alg, nobel, nobel_span, n_pool, n_stream, hooks);

      if (!ton::test::is_asan_active()) {
        REQUIRE(ton::test::tracked_allocations() == 0);
      }

      REQUIRE(result.records.size() == 1000);
      REQUIRE(result.summary.num_requests == 1000);
      REQUIRE(result.summary.total_granted > 0);
      REQUIRE(result.summary.first_denial_idx <= 1000);
      REQUIRE(result.summary.refcounts_exact);

      REQUIRE(result.summary.mean_latency_full_us > 0.0);
      REQUIRE(result.summary.mean_latency_pre_denial_us > 0.0);

      REQUIRE(ton::SpectralTelemetry::verify_refcounts_exact(result.state));
      REQUIRE(ton::SpectralTelemetry::evaluate(result.state) ==
              ton::SpectralTelemetry::evaluate_full_scan(result.state));

      if (ton::algorithm_regime(alg) == ton::FailureRegime::Simultaneous) {
        ton::test::audit_simultaneous_state_invariants(result.state, n_pool);
      } else {
        ton::test::audit_sequential_state_invariants(result.state, n_pool);
      }
    }
  }

  SECTION("Germany50 Span multi-algorithm scale verification") {
    const auto germany = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "germany50.xml");
    const auto germany_span =
        ton::ScenarioSet::enumerate(germany, ton::CutModel::Span);

#if defined(NDEBUG)
    constexpr size_t kGermanyReqs = 1000;
#else
    constexpr size_t kGermanyReqs = 200;
#endif

    const auto g_stream = ton::load_request_stream_csv(
        ton::test::repo_root() / "data" / "streams" /
            "germany50_1d1m_seed_01.csv",
        kGermanyReqs);
    REQUIRE(g_stream.size() == kGermanyReqs);

    ton::PathPool g_pool(germany, ton::DistanceMetric::GeographicKm, 0.10, 10);
    g_pool.prewarm_for_stream(g_stream, germany_span);

    for (const ton::AlgorithmId alg : ton::ALL_ALGORITHMS) {
      ton::OnlineLoopHooks hooks{};
      if (!ton::test::is_asan_active()) {
        hooks.on_before_loop = ton::test::arm_zero_alloc_monitor;
        hooks.on_after_loop = ton::test::disarm_zero_alloc_monitor;
      }

      const ton::ProvisioningRunResult result = ton::run_provisioning_algorithm(
          alg, germany, germany_span, g_pool, g_stream, hooks);

      if (!ton::test::is_asan_active()) {
        REQUIRE(ton::test::tracked_allocations() == 0);
      }

      REQUIRE(result.records.size() == kGermanyReqs);
      REQUIRE(result.summary.refcounts_exact);
      REQUIRE(ton::SpectralTelemetry::verify_refcounts_exact(result.state));
      REQUIRE(ton::SpectralTelemetry::evaluate(result.state) ==
              ton::SpectralTelemetry::evaluate_full_scan(result.state));

      if (ton::algorithm_regime(alg) == ton::FailureRegime::Simultaneous) {
        ton::test::audit_simultaneous_state_invariants(result.state, g_pool);
      } else {
        ton::test::audit_sequential_state_invariants(result.state, g_pool);
      }
    }
  }
}
