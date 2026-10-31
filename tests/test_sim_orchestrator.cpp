#include <vector>

#include "test_helpers.hpp"
#include "ton/orchestration/sim_orchestrator.hpp"
#include "ton/solvers/dsatur_solver.hpp"
#include "ton/solvers/greedy_solver.hpp"
#include "ton/solvers/no_reroute_solver.hpp"

TEST_CASE("SimOrchestrator on Nobel-EU with 5-Invariant verification",
          "[orchestration][sim]") {
  const auto nobel = ton::Graph::from_sndlib_xml(
      ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
  const auto nobel_span =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
  const auto stream =
      ton::load_request_stream_csv(ton::test::repo_root() / "data" / "streams" /
                                       "nobel_eu_realistic_seed_01.csv",
                                   300);
  REQUIRE(stream.size() == 300);

  ton::PathPool pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
  pool.prewarm_for_stream(stream, nobel_span);

  SECTION("SimRDF (DsaturSolver) and deterministic reset") {
    ton::NetworkState sim_rdf_state(nobel, nobel_span,
                                    ton::FailureRegime::Simultaneous, 300);
    ton::SimOrchestrator<ton::DsaturSolver> sim_rdf(nobel, nobel_span, pool,
                                                    300);
    uint32_t sim_rdf_denied = 0;
    for (const auto& req : stream) {
      const auto prev_metrics = ton::SpectralTelemetry::evaluate(sim_rdf_state);
      const uint16_t prev_adm = sim_rdf_state.num_admitted();
      const auto res = sim_rdf.admit_request(req, sim_rdf_state);
      if (!res.admitted) {
        ++sim_rdf_denied;
        REQUIRE(sim_rdf_state.num_admitted() == prev_adm);
        REQUIRE(ton::SpectralTelemetry::evaluate(sim_rdf_state) ==
                prev_metrics);
      }
    }
    REQUIRE(sim_rdf_state.num_admitted() > 50);
    REQUIRE(sim_rdf_denied > 0);
    ton::test::audit_simultaneous_state_invariants(sim_rdf_state, pool);

    const uint16_t first_run_adm = sim_rdf_state.num_admitted();
    const auto first_run_metrics =
        ton::SpectralTelemetry::evaluate(sim_rdf_state);
    sim_rdf_state.reset();
    sim_rdf.reset();
    for (const auto& req : stream) {
      (void)sim_rdf.admit_request(req, sim_rdf_state);
    }
    REQUIRE(sim_rdf_state.num_admitted() == first_run_adm);
    REQUIRE(ton::SpectralTelemetry::evaluate(sim_rdf_state) ==
            first_run_metrics);
  }

  SECTION("GreedySimRDF (GreedySolver)") {
    ton::NetworkState greedy_sim_state(nobel, nobel_span,
                                       ton::FailureRegime::Simultaneous, 300);
    ton::SimOrchestrator<ton::GreedySolver> greedy_sim(nobel, nobel_span, pool,
                                                       300);
    for (const auto& req : stream) {
      (void)greedy_sim.admit_request(req, greedy_sim_state);
    }
    REQUIRE(greedy_sim_state.num_admitted() > 50);
    ton::test::audit_simultaneous_state_invariants(greedy_sim_state, pool);
  }

  SECTION("NoRerouteSimRDF (NoRerouteSolver) and frozen backup immutability") {
    ton::NetworkState nr_sim_state(nobel, nobel_span,
                                   ton::FailureRegime::Simultaneous, 300);
    ton::SimOrchestrator<ton::NoRerouteSolver> nr_sim(nobel, nobel_span, pool,
                                                      300);

    struct FrozenBackupRecord {
      ton::ScenarioId f;
      ton::RequestId r;
      ton::Lightpath lp;
    };
    std::vector<FrozenBackupRecord> frozen_records;

    for (const auto& req : stream) {
      const auto res = nr_sim.admit_request(req, nr_sim_state);
      if (res.admitted) {
        for (const auto& sc : nobel_span.sim_scenarios()) {
          if (pool.path_intersects(res.primary.path_id, sc.cut_arcs)) {
            const ton::Lightpath lp =
                nr_sim_state.find_sim_backup(sc.id, req.id);
            REQUIRE(lp.valid());
            frozen_records.push_back(
                FrozenBackupRecord{.f = sc.id, .r = req.id, .lp = lp});
          }
        }
      }
    }
    REQUIRE(nr_sim_state.num_admitted() > 40);
    for (const auto& rec : frozen_records) {
      REQUIRE(nr_sim_state.find_sim_backup(rec.f, rec.r) == rec.lp);
    }
    ton::test::audit_simultaneous_state_invariants(nr_sim_state, pool);
  }
}
