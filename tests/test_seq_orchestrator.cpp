#include <vector>

#include "test_helpers.hpp"
#include "ton/orchestration/seq_orchestrator.hpp"
#include "ton/solvers/dsatur_solver.hpp"
#include "ton/solvers/greedy_solver.hpp"
#include "ton/solvers/no_reroute_solver.hpp"

TEST_CASE(
    "SeqOrchestrator on Nobel-EU and Germany50 with 5-Invariant verification",
    "[orchestration][seq]") {
  const auto nobel = ton::Graph::from_sndlib_xml(
      ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
  const auto nobel_span =
      ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
  const auto n_stream =
      ton::load_request_stream_csv(ton::test::repo_root() / "data" / "streams" /
                                       "nobel_eu_realistic_seed_01.csv",
                                   260);
  REQUIRE(n_stream.size() == 260);

  ton::PathPool n_pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
  n_pool.prewarm_for_stream(n_stream, nobel_span);

  SECTION("SeqRDF (DsaturSolver) and deterministic reset") {
    ton::NetworkState seq_rdf_state(nobel, nobel_span,
                                    ton::FailureRegime::Sequential, 260);
    ton::SeqOrchestrator<ton::DsaturSolver> seq_rdf(nobel, nobel_span, n_pool,
                                                    260);
    uint32_t seq_rdf_denied = 0;
    for (const auto& req : n_stream) {
      const auto prev_metrics = ton::SpectralTelemetry::evaluate(seq_rdf_state);
      const uint16_t prev_adm = seq_rdf_state.num_admitted();
      const auto res = seq_rdf.admit_request(req, seq_rdf_state);
      if (!res.admitted) {
        ++seq_rdf_denied;
        REQUIRE(seq_rdf_state.num_admitted() == prev_adm);
        REQUIRE(ton::SpectralTelemetry::evaluate(seq_rdf_state) ==
                prev_metrics);
      }
    }
    REQUIRE(seq_rdf_state.num_admitted() > 50);
    REQUIRE(seq_rdf_denied > 0);
    ton::test::audit_sequential_state_invariants(seq_rdf_state, n_pool);

    const uint16_t first_adm = seq_rdf_state.num_admitted();
    const auto first_metrics = ton::SpectralTelemetry::evaluate(seq_rdf_state);
    seq_rdf_state.reset();
    seq_rdf.reset();
    for (const auto& req : n_stream) {
      (void)seq_rdf.admit_request(req, seq_rdf_state);
    }
    REQUIRE(seq_rdf_state.num_admitted() == first_adm);
    REQUIRE(ton::SpectralTelemetry::evaluate(seq_rdf_state) == first_metrics);
  }

  SECTION("GreedySeqRDF (GreedySolver)") {
    ton::NetworkState greedy_seq_state(nobel, nobel_span,
                                       ton::FailureRegime::Sequential, 260);
    ton::SeqOrchestrator<ton::GreedySolver> greedy_seq(nobel, nobel_span,
                                                       n_pool, 260);
    for (const auto& req : n_stream) {
      (void)greedy_seq.admit_request(req, greedy_seq_state);
    }
    REQUIRE(greedy_seq_state.num_admitted() > 50);
    ton::test::audit_sequential_state_invariants(greedy_seq_state, n_pool);
  }

  SECTION("NoRerouteSeqRDF (NoRerouteSolver) and frozen backup immutability") {
    ton::NetworkState nr_seq_state(nobel, nobel_span,
                                   ton::FailureRegime::Sequential, 260);
    ton::SeqOrchestrator<ton::NoRerouteSolver> nr_seq(nobel, nobel_span, n_pool,
                                                      260);

    struct FrozenSeqRecord {
      ton::StateDomain domain;
      uint32_t state_id;
      ton::RequestId r;
      ton::Lightpath lp;
    };
    std::vector<FrozenSeqRecord> frozen_seq;

    for (const auto& req : n_stream) {
      const auto res = nr_seq.admit_request(req, nr_seq_state);
      if (res.admitted) {
        for (const ton::FailureUnitId x1 : nobel_span.active_stage1_units()) {
          if (n_pool.path_intersects(res.primary.path_id,
                                     nobel_span.unit_cut_arcs(x1))) {
            const ton::Lightpath lp =
                nr_seq_state.find_seq_stage1_backup(x1, req.id);
            REQUIRE(lp.valid());
            frozen_seq.push_back(FrozenSeqRecord{
                .domain = ton::StateDomain::SeqStage1,
                .state_id = x1,
                .r = req.id,
                .lp = lp,
            });
          }
        }
        for (const auto& sc : nobel_span.seq_scenarios()) {
          if (n_pool.path_intersects(res.primary.path_id, sc.total_cut)) {
            const ton::Lightpath lp =
                nr_seq_state.find_seq_stage2_backup(sc.id, req.id);
            REQUIRE(lp.valid());
            frozen_seq.push_back(FrozenSeqRecord{
                .domain = ton::StateDomain::SeqStage2,
                .state_id = sc.id,
                .r = req.id,
                .lp = lp,
            });
          }
        }
      }
    }
    REQUIRE(nr_seq_state.num_admitted() >= 10);
    for (const auto& rec : frozen_seq) {
      if (rec.domain == ton::StateDomain::SeqStage1) {
        REQUIRE(nr_seq_state.find_seq_stage1_backup(
                    static_cast<ton::FailureUnitId>(rec.state_id), rec.r) ==
                rec.lp);
      } else {
        REQUIRE(nr_seq_state.find_seq_stage2_backup(rec.state_id, rec.r) ==
                rec.lp);
      }
    }
    ton::test::audit_sequential_state_invariants(nr_seq_state, n_pool);
  }

  SECTION("Germany50 Span Sequential cuts (88 Stage-1 units, 7,634 Stage-2)") {
    const auto germany = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "germany50.xml");
    const auto germany_span =
        ton::ScenarioSet::enumerate(germany, ton::CutModel::Span);
    const auto g_stream = ton::load_request_stream_csv(
        ton::test::repo_root() / "data" / "streams" /
            "germany50_1d1m_seed_01.csv",
        120);
    REQUIRE(g_stream.size() == 120);

    ton::PathPool g_pool(germany, ton::DistanceMetric::GeographicKm, 0.10, 10);
    g_pool.prewarm_for_stream(g_stream, germany_span);

    ton::NetworkState g_seq_state(germany, germany_span,
                                  ton::FailureRegime::Sequential, 120);
    ton::SeqOrchestrator<ton::DsaturSolver> g_seq_rdf(germany, germany_span,
                                                      g_pool, 120);
    for (const auto& req : g_stream) {
      (void)g_seq_rdf.admit_request(req, g_seq_state);
    }
    REQUIRE(g_seq_state.num_admitted() > 50);
    ton::test::audit_sequential_state_invariants(g_seq_state, g_pool);
  }
}
