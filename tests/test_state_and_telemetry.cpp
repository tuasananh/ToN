#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_helpers.hpp"

TEST_CASE("NetworkState, StagingArena, and sub-0.5us SpectralTelemetry",
          "[orchestration][telemetry]") {
  SECTION("Simultaneous state, O(1) rollback, and exact telemetry equations") {
    const auto nobel = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
    const auto nobel_span =
        ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);

    ton::PathPool pool(nobel, ton::DistanceMetric::GeographicKm, 0.10, 10);
    pool.prewarm_all_healthy();

    ton::NetworkState sim_state(nobel, nobel_span,
                                ton::FailureRegime::Simultaneous, 64);
    auto sim_staged = ton::StagedAssignments::for_regime(
        nobel_span, ton::FailureRegime::Simultaneous, 64);

    const auto empty_metrics = ton::SpectralTelemetry::evaluate(sim_state);
    REQUIRE(empty_metrics ==
            ton::SpectralTelemetry::evaluate_full_scan(sim_state));
    REQUIRE(empty_metrics.h_prim_slots == 0);
    REQUIRE(empty_metrics.h_back_slots == 0);
    REQUIRE(empty_metrics.overhead_ratio == 0.0);
    REQUIRE(ton::SpectralTelemetry::verify_refcounts_exact(sim_state));

    const ton::Request req0{.id = 0, .src = 0, .dst = 10};
    const ton::Request req1{.id = 1, .src = 10, .dst = 27};
    const ton::PathSlice p0_slice = pool.find_healthy(req0.src, req0.dst);
    const ton::PathSlice p1_slice = pool.find_healthy(req1.src, req1.dst);
    REQUIRE_FALSE(p0_slice.empty());
    REQUIRE_FALSE(p1_slice.empty());

    const ton::PathId p0_prim = p0_slice[0];
    const ton::PathId p1_prim = p1_slice[0];
    REQUIRE(sim_state.is_primary_clash_free(pool.path_arcs(p0_prim), 0));

    const ton::ArcId first_a0 = pool.path_arcs(p0_prim).front();
    const auto cutting_p0 = nobel_span.sim_scenarios_cutting_arc(first_a0);
    REQUIRE(cutting_p0.size() >= 2);
    const ton::ScenarioId f_a = cutting_p0[0];
    const ton::ScenarioId f_b = cutting_p0[1];

    const ton::PathId b0_fa = pool.get_or_compute(
        nobel_span.sim_scenario(f_a).cut_arcs, req0.src, req0.dst)[0];
    const ton::PathSlice b0_fb_slice = pool.get_or_compute(
        nobel_span.sim_scenario(f_b).cut_arcs, req0.src, req0.dst);
    const ton::PathId b0_fb = b0_fb_slice[0];

    const ton::BackupAssignment ba_fa_w31{
        .path_id = b0_fa, .req_id = 0, .wavelength = 31};
    const ton::BackupAssignment ba_fb_w31{
        .path_id = b0_fb, .req_id = 0, .wavelength = 31};
    sim_staged.stage_replace(ton::StateDomain::Sim, f_a,
                             std::span{&ba_fa_w31, 1});
    const auto cp = sim_staged.checkpoint();
    sim_staged.stage_append(ton::StateDomain::Sim, f_b, ba_fb_w31);
    REQUIRE(sim_staged.num_deltas() == 2);
    REQUIRE(sim_staged.num_entries() == 2);

    sim_staged.rollback_to(cp);
    REQUIRE(sim_staged.num_deltas() == 1);
    REQUIRE(sim_staged.num_entries() == 1);
    sim_staged.rollback();
    REQUIRE(sim_staged.empty());
    REQUIRE(sim_state.num_admitted() == 0);

    sim_staged.stage_replace(ton::StateDomain::Sim, f_a,
                             std::span{&ba_fa_w31, 1});
    sim_staged.stage_replace(ton::StateDomain::Sim, f_b,
                             std::span{&ba_fb_w31, 1});
    sim_state.commit(req0, ton::Lightpath{.path_id = p0_prim, .wavelength = 0},
                     sim_staged, pool);

    REQUIRE(sim_state.num_admitted() == 1);
    REQUIRE(sim_state.is_admitted(0));
    REQUIRE_FALSE(sim_state.is_primary_clash_free(pool.path_arcs(p0_prim), 0));
    REQUIRE(sim_state.is_primary_clash_free(pool.path_arcs(p0_prim), 1));
    REQUIRE(sim_state.find_sim_backup(f_a, 0) ==
            (ton::Lightpath{.path_id = b0_fa, .wavelength = 31}));
    REQUIRE(ton::SpectralTelemetry::verify_refcounts_exact(sim_state));
    REQUIRE(ton::SpectralTelemetry::evaluate(sim_state) ==
            ton::SpectralTelemetry::evaluate_full_scan(sim_state));

    std::vector<uint32_t> excl(nobel_span.sim_scenarios().size(), 0);
    std::vector<uint32_t> visit(nobel_span.sim_scenarios().size(), 0);
    std::vector<uint32_t> hits(nobel_span.sim_scenarios().size(), 0);
    REQUIRE(sim_state.has_interfering_state(
        ton::StateDomain::Sim, pool.path_arcs(b0_fa), 31, excl, 1));
    REQUIRE_FALSE(sim_state.has_interfering_state(
        ton::StateDomain::Sim, pool.path_arcs(b0_fa), 30, excl, 1));
    const uint32_t n_hits_before = sim_state.collect_interfering_states(
        ton::StateDomain::Sim, pool.path_arcs(b0_fa), 31, excl, 1, visit, 1,
        hits);
    REQUIRE(n_hits_before >= 1);

    sim_staged.rollback();
    const ton::BackupAssignment ba_fb_w0{
        .path_id = b0_fb, .req_id = 0, .wavelength = 0};
    sim_staged.stage_replace(ton::StateDomain::Sim, f_b,
                             std::span{&ba_fb_w0, 1});
    const ton::ScenarioId f_c = nobel_span.sim_scenarios_cutting_arc(
        pool.path_arcs(p1_prim).front())[0];
    const ton::PathId b1_fc = pool.get_or_compute(
        nobel_span.sim_scenario(f_c).cut_arcs, req1.src, req1.dst)[0];
    if (f_c != f_a && f_c != f_b) {
      sim_staged.stage_append(
          ton::StateDomain::Sim, f_c,
          ton::BackupAssignment{
              .path_id = b1_fc, .req_id = 1, .wavelength = 30});
    }
    sim_state.commit(req1, ton::Lightpath{.path_id = p1_prim, .wavelength = 31},
                     sim_staged, pool);

    REQUIRE(sim_state.find_sim_backup(f_b, 0) ==
            (ton::Lightpath{.path_id = b0_fb, .wavelength = 0}));
    REQUIRE(ton::SpectralTelemetry::verify_refcounts_exact(sim_state));
    const auto m_after = ton::SpectralTelemetry::evaluate(sim_state);
    const auto m_scan = ton::SpectralTelemetry::evaluate_full_scan(sim_state);
    REQUIRE(m_after == m_scan);
    REQUIRE(m_after.h_pure_slots + m_after.h_stub_slots ==
            m_after.h_back_slots);
    REQUIRE(std::abs(m_after.u_total_pct -
                     (m_after.u_prim_pct + m_after.u_pure_pct)) < 1e-12);
    REQUIRE(std::abs(m_after.overhead_ratio -
                     static_cast<double>(m_after.h_pure_slots) /
                         static_cast<double>(m_after.h_prim_slots)) < 1e-12);

    std::ostringstream oss;
    ton::SpectralTelemetry::write_csv_header(oss);
    ton::SpectralTelemetry::write_csv_row(oss,
                                          ton::StepRecord{
                                              .t = 1,
                                              .src = req1.src,
                                              .dst = req1.dst,
                                              .admitted = true,
                                              .latency_us = 4.25,
                                              .k_adm = sim_state.num_admitted(),
                                              .spectral = m_after,
                                          });
    const std::string csv_str = oss.str();
    REQUIRE(csv_str.starts_with(
        "t,src,dst,admitted,latency_us,k_adm,h_prim_slots,h_back_slots,"
        "h_pure_slots,h_stub_slots,u_prim_pct,u_back_pct,u_pure_pct,"
        "u_stub_pct,u_total_pct,overhead_ratio\n1,"));

    sim_state.reset();
    REQUIRE(sim_state.num_admitted() == 0);
    REQUIRE(ton::SpectralTelemetry::verify_refcounts_exact(sim_state));
    REQUIRE(ton::SpectralTelemetry::evaluate(sim_state).h_back_slots == 0);
  }

  SECTION(
      "Full-scale Sequential NetworkState on Germany50 Directional and <0.5us "
      "telemetry") {
    const auto germany = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "germany50.xml");
    const auto germany_dir =
        ton::ScenarioSet::enumerate(germany, ton::CutModel::Directional);
    ton::PathPool g_pool(germany, ton::DistanceMetric::GeographicKm, 0.10, 10);

    ton::NetworkState seq_state(germany, germany_dir,
                                ton::FailureRegime::Sequential, 32);
    auto seq_staged = ton::StagedAssignments::for_regime(
        germany_dir, ton::FailureRegime::Sequential, 32);

    const ton::Request greq{.id = 0, .src = 3, .dst = 16};
    const ton::PathId g_prim = g_pool.get_or_compute_healthy(3, 16)[0];
    const ton::ArcId g_cut_arc = g_pool.path_arcs(g_prim).front();
    const ton::FailureUnitId gx1 = germany_dir.arc_to_unit(g_cut_arc);
    REQUIRE(germany_dir.is_active_stage1_unit(gx1));

    const ton::PathId g_s1_bak =
        g_pool.get_or_compute(germany_dir.unit_cut_arcs(gx1), 3, 16)[0];
    const ton::BackupAssignment s1_ba{
        .path_id = g_s1_bak, .req_id = 0, .wavelength = 31};
    seq_staged.stage_replace(ton::StateDomain::SeqStage1, gx1,
                             std::span{&s1_ba, 1});

    for (const auto& seq_sc : germany_dir.seq_group(gx1)) {
      const ton::PathId s2_pid =
          g_pool.path_intersects(g_s1_bak, seq_sc.stage2_cut)
              ? g_pool.get_or_compute(seq_sc.total_cut, 3, 16)[0]
              : g_s1_bak;
      seq_staged.stage_append(
          ton::StateDomain::SeqStage2, seq_sc.id,
          ton::BackupAssignment{
              .path_id = s2_pid, .req_id = 0, .wavelength = 31});
    }

    seq_state.commit(greq, ton::Lightpath{.path_id = g_prim, .wavelength = 0},
                     seq_staged, g_pool);
    REQUIRE(ton::SpectralTelemetry::verify_refcounts_exact(seq_state));
    REQUIRE(ton::SpectralTelemetry::evaluate(seq_state) ==
            ton::SpectralTelemetry::evaluate_full_scan(seq_state));

    uint64_t sink = 0;
    constexpr int kIters = 2000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
      const auto sm = ton::SpectralTelemetry::evaluate(seq_state);
      sink += sm.h_prim_slots + sm.h_pure_slots;
    }
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE(sink > 0);
    const double avg_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / kIters;
#if defined(NDEBUG)
    REQUIRE(avg_us < 0.5);
#else
    REQUIRE(avg_us < 50.0);
#endif
  }

  SECTION("Master summary CSV and JSON export schema") {
    const std::string_view header = ton::MASTER_SUMMARY_CSV_HEADER;
    // Count commas in header: 23 columns means exactly 22 commas
    size_t comma_count = 0;
    for (char c : header) {
      if (c == ',') ++comma_count;
    }
    REQUIRE(comma_count == 22);

    const std::string formatted =
        ton::SpectralTelemetry::format_summary_csv_row(
            "exp1", "nobel-eu", "span", "realistic", "s01",
            ton::AlgorithmId::SimRDF, ton::FailureRegime::Simultaneous,
            "DSATUR", 131, 313, 0.687, 2229.22, 3120.45, 1962.94, 1840.12,
            28.47, 70.54, 2.4779, 28.47, 70.54, 19.21, 2.4779, true);

    size_t row_commas = 0;
    for (char c : formatted) {
      if (c == ',') ++row_commas;
    }
    REQUIRE(row_commas == 22);
    REQUIRE(formatted.ends_with(",true\n"));

    const std::filesystem::path temp_csv =
        std::filesystem::temp_directory_path() / "ton_test_summary_append.csv";
    std::filesystem::remove(temp_csv);

    ton::SpectralTelemetry::append_summary_csv_row(
        temp_csv, "exp1", "nobel-eu", "span", "realistic", "s01",
        ton::AlgorithmId::SimRDF, ton::FailureRegime::Simultaneous, "DSATUR",
        131, 313, 0.687, 2229.22, 3120.45, 1962.94, 1840.12, 28.47, 70.54,
        2.4779, 28.47, 70.54, 19.21, 2.4779, true);

    ton::SpectralTelemetry::append_summary_csv_row(
        temp_csv, "exp1", "nobel-eu", "span", "realistic", "s01",
        ton::AlgorithmId::SeqRDF, ton::FailureRegime::Sequential, "DSATUR", 126,
        308, 0.692, 1918.71, 2800.12, 2799.77, 2600.50, 28.28, 70.66, 2.4987,
        28.28, 70.66, 18.83, 2.4987, true);

    std::ifstream ifs(temp_csv);
    REQUIRE(ifs.is_open());
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(ifs, line)) {
      if (!line.empty()) lines.push_back(line);
    }
    REQUIRE(lines.size() == 3);  // Header + 2 data rows
    REQUIRE(lines[0] ==
            "exp,topology,cut_model,traffic,seq_id,algorithm,regime,heuristic,"
            "first_denial_idx,total_granted,blocking_prob,mean_latency_pre_"
            "denial_us,p95_latency_pre_denial_us,mean_latency_full_us,mean_"
            "latency_rejected_us,u_prim_at_denial,u_pure_at_denial,overhead_at_"
            "denial,u_prim_final,u_pure_final,u_stub_final,overhead_final,"
            "invariants_verified");
    REQUIRE(lines[1].find("SimRDF") != std::string::npos);
    REQUIRE(lines[2].find("SeqRDF") != std::string::npos);
    std::filesystem::remove(temp_csv);

    // Test write_telemetry_json
    const std::filesystem::path temp_json =
        std::filesystem::temp_directory_path() / "ton_test_telemetry.json";
    std::filesystem::remove(temp_json);

    std::vector<ton::RunSummary> summaries;
    ton::RunSummary s1{};
    s1.algorithm = ton::AlgorithmId::SimRDF;
    s1.regime = ton::FailureRegime::Simultaneous;
    s1.first_denial_idx = 131;
    s1.total_granted = 313;
    s1.blocking_prob = 0.687;
    s1.mean_latency_pre_denial_us = 2229.22;
    s1.p95_latency_pre_denial_us = 3120.45;
    s1.mean_latency_full_us = 1962.94;
    s1.mean_latency_rejected_us = 1840.12;
    s1.refcounts_exact = true;
    summaries.push_back(s1);

    std::vector<uint8_t> inv_verified = {1};

    ton::write_telemetry_json(temp_json, "exp1", "nobel-eu", "span",
                              "realistic", "s01", summaries, inv_verified);

    REQUIRE(std::filesystem::exists(temp_json));
    std::ifstream json_ifs(temp_json);
    REQUIRE(json_ifs.is_open());
    std::string json_content((std::istreambuf_iterator<char>(json_ifs)),
                             std::istreambuf_iterator<char>());
    REQUIRE(json_content.find("\"exp\": \"exp1\"") != std::string::npos);
    REQUIRE(json_content.find("\"algorithm\": \"SimRDF\"") !=
            std::string::npos);
    REQUIRE(json_content.find("\"invariants_verified\": true") !=
            std::string::npos);
    std::filesystem::remove(temp_json);
  }
}
