#include "ton/orchestration/provisioning_harness.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <string>

namespace ton {

bool try_parse_algorithm_id(std::string_view name,
                            AlgorithmId& out_alg) noexcept {
  for (const AlgorithmId alg : ALL_ALGORITHMS) {
    if (algorithm_name(alg) == name) {
      out_alg = alg;
      return true;
    }
  }
  return false;
}

std::vector<Request> load_request_stream_csv(
    const std::filesystem::path& csv_path, size_t max_count) {
  std::ifstream ifs(csv_path);
  if (!ifs.is_open()) {
    throw std::runtime_error("Failed to open request stream CSV: " +
                             csv_path.string());
  }

  std::string line;
  if (!std::getline(ifs, line)) {
    throw std::runtime_error("Empty request stream CSV: " + csv_path.string());
  }

  std::vector<Request> stream;
  stream.reserve(max_count);
  while (stream.size() < max_count && std::getline(ifs, line)) {
    if (line.empty()) {
      continue;
    }
    const size_t c1 = line.find(',');
    if (c1 == std::string::npos) {
      throw std::runtime_error("Malformed row in request stream CSV: " + line);
    }
    const size_t c2 = line.find(',', c1 + 1u);
    if (c2 == std::string::npos) {
      throw std::runtime_error("Malformed row in request stream CSV: " + line);
    }
    const size_t c3 = line.find(',', c2 + 1u);

    const auto t = static_cast<RequestId>(std::stoul(line.substr(0, c1)));
    const auto src =
        static_cast<NodeId>(std::stoul(line.substr(c1 + 1u, c2 - (c1 + 1u))));
    const auto dst = static_cast<NodeId>(std::stoul(
        c3 == std::string::npos ? line.substr(c2 + 1u)
                                : line.substr(c2 + 1u, c3 - (c2 + 1u))));
    stream.push_back(Request{.id = t, .src = src, .dst = dst});
  }
  return stream;
}

RunSummary summarize_provisioning_run(AlgorithmId algorithm,
                                      std::span<const StepRecord> records,
                                      bool refcounts_exact,
                                      uint64_t theorem1_bypass_steps,
                                      uint64_t theorem1_fc_steps) {
  RunSummary summary{};
  summary.algorithm = algorithm;
  summary.regime = algorithm_regime(algorithm);
  summary.num_requests = static_cast<uint32_t>(records.size());
  summary.first_denial_idx = summary.num_requests;
  summary.refcounts_exact = refcounts_exact;
  summary.dsatur_theorem1_bypass_steps = theorem1_bypass_steps;
  summary.dsatur_theorem1_fc_checked_steps = theorem1_fc_steps;

  if (records.empty()) {
    return summary;
  }

  double sum_full_us = 0.0;
  double sum_rejected_us = 0.0;
  uint32_t num_rejected = 0;

  for (size_t i = 0; i < records.size(); ++i) {
    const StepRecord& rec = records[i];
    sum_full_us += rec.latency_us;
    if (rec.admitted) {
      ++summary.total_granted;
    } else {
      if (summary.first_denial_idx == summary.num_requests) {
        summary.first_denial_idx = static_cast<uint32_t>(i);
      }
      sum_rejected_us += rec.latency_us;
      ++num_rejected;
    }
  }

  summary.blocking_prob = 1.0 - (static_cast<double>(summary.total_granted) /
                                 static_cast<double>(summary.num_requests));
  summary.mean_latency_full_us =
      sum_full_us / static_cast<double>(summary.num_requests);
  summary.mean_latency_rejected_us =
      (num_rejected > 0u)
          ? (sum_rejected_us / static_cast<double>(num_rejected))
          : 0.0;

  if (summary.first_denial_idx > 0u) {
    std::vector<double> pre_latencies;
    pre_latencies.reserve(summary.first_denial_idx);
    double sum_pre_us = 0.0;
    for (uint32_t i = 0; i < summary.first_denial_idx; ++i) {
      sum_pre_us += records[i].latency_us;
      pre_latencies.push_back(records[i].latency_us);
    }
    summary.mean_latency_pre_denial_us =
        sum_pre_us / static_cast<double>(summary.first_denial_idx);
    std::sort(pre_latencies.begin(), pre_latencies.end());
    const size_t p95_idx = std::min<size_t>(
        pre_latencies.size() - 1u,
        static_cast<size_t>(
            std::ceil(0.95 * static_cast<double>(pre_latencies.size()))) -
            1u);
    summary.p95_latency_pre_denial_us = pre_latencies[p95_idx];
    summary.spectral_at_denial =
        records[summary.first_denial_idx - 1u].spectral;
  }

  summary.spectral_final = records.back().spectral;
  return summary;
}

namespace {

[[nodiscard]] uint16_t compute_max_request_slot(
    std::span<const Request> requests) noexcept {
  uint32_t max_id = static_cast<uint32_t>(requests.size());
  for (const Request& req : requests) {
    max_id = std::max<uint32_t>(max_id, static_cast<uint32_t>(req.id) + 1u);
  }
  return static_cast<uint16_t>(std::min<uint32_t>(
      std::max<uint32_t>(1u, max_id), static_cast<uint32_t>(INVALID_REQUEST)));
}

template <typename Orchestrator>
ProvisioningRunResult run_with_orchestrator(AlgorithmId algorithm,
                                            const Graph& graph,
                                            const ScenarioSet& scenarios,
                                            const PathPool& prewarmed_pool,
                                            std::span<const Request> requests,
                                            OnlineLoopHooks hooks) {
  const uint16_t max_req = compute_max_request_slot(requests);
  NetworkState state(graph, scenarios, algorithm_regime(algorithm), max_req);
  Orchestrator orchestrator(graph, scenarios, prewarmed_pool, max_req);
  std::vector<StepRecord> records(requests.size(), StepRecord{});

  execute_online_provisioning_loop(orchestrator, state, requests, records,
                                   hooks);

  const bool refcounts_ok = SpectralTelemetry::verify_refcounts_exact(state) &&
                            (SpectralTelemetry::evaluate(state) ==
                             SpectralTelemetry::evaluate_full_scan(state));

  RunSummary summary = summarize_provisioning_run(
      algorithm, records, refcounts_ok,
      orchestrator.scratchpad().theorem1_bypass_steps,
      orchestrator.scratchpad().theorem1_fc_checked_steps);

  return ProvisioningRunResult{
      .state = std::move(state),
      .records = std::move(records),
      .summary = summary,
  };
}

}  // namespace

ProvisioningRunResult run_provisioning_algorithm(
    AlgorithmId algorithm, const Graph& graph, const ScenarioSet& scenarios,
    const PathPool& prewarmed_pool, std::span<const Request> requests,
    OnlineLoopHooks hooks) {
  switch (algorithm) {
    case AlgorithmId::SimRDF:
      return run_with_orchestrator<SimRdfOrchestrator>(
          algorithm, graph, scenarios, prewarmed_pool, requests, hooks);
    case AlgorithmId::SeqRDF:
      return run_with_orchestrator<SeqRdfOrchestrator>(
          algorithm, graph, scenarios, prewarmed_pool, requests, hooks);
    case AlgorithmId::GreedySimRDF:
      return run_with_orchestrator<GreedySimRdfOrchestrator>(
          algorithm, graph, scenarios, prewarmed_pool, requests, hooks);
    case AlgorithmId::GreedySeqRDF:
      return run_with_orchestrator<GreedySeqRdfOrchestrator>(
          algorithm, graph, scenarios, prewarmed_pool, requests, hooks);
    case AlgorithmId::NoRerouteSimRDF:
      return run_with_orchestrator<NoRerouteSimRdfOrchestrator>(
          algorithm, graph, scenarios, prewarmed_pool, requests, hooks);
    case AlgorithmId::NoRerouteSeqRDF:
      return run_with_orchestrator<NoRerouteSeqRdfOrchestrator>(
          algorithm, graph, scenarios, prewarmed_pool, requests, hooks);
  }
  throw std::invalid_argument("Unsupported AlgorithmId");
}

void run_provisioning_suite(
    std::span<const AlgorithmId> algorithms, const Graph& graph,
    const ScenarioSet& scenarios, const PathPool& prewarmed_pool,
    std::span<const Request> requests,
    const std::function<void(const ProvisioningRunView&)>& on_result,
    OnlineLoopHooks hooks) {
  if (algorithms.empty()) {
    return;
  }

  const uint16_t max_req = compute_max_request_slot(requests);

  std::unique_ptr<NetworkState> sim_state;
  std::unique_ptr<NetworkState> seq_state;

  std::unique_ptr<SimRdfOrchestrator> sim_rdf;
  std::unique_ptr<SeqRdfOrchestrator> seq_rdf;
  std::unique_ptr<GreedySimRdfOrchestrator> greedy_sim_rdf;
  std::unique_ptr<GreedySeqRdfOrchestrator> greedy_seq_rdf;
  std::unique_ptr<NoRerouteSimRdfOrchestrator> no_reroute_sim_rdf;
  std::unique_ptr<NoRerouteSeqRdfOrchestrator> no_reroute_seq_rdf;

  std::vector<StepRecord> records(requests.size(), StepRecord{});

  auto get_sim_state = [&]() -> NetworkState& {
    if (!sim_state) {
      sim_state = std::make_unique<NetworkState>(
          graph, scenarios, FailureRegime::Simultaneous, max_req);
    }
    return *sim_state;
  };

  auto get_seq_state = [&]() -> NetworkState& {
    if (!seq_state) {
      seq_state = std::make_unique<NetworkState>(
          graph, scenarios, FailureRegime::Sequential, max_req);
    }
    return *seq_state;
  };

  auto run_single = [&](auto& orchestrator, NetworkState& state,
                        AlgorithmId algorithm) {
    state.reset();
    orchestrator.reset();
    std::fill(records.begin(), records.end(), StepRecord{});

    execute_online_provisioning_loop(orchestrator, state, requests, records,
                                     hooks);

    const bool refcounts_ok =
        SpectralTelemetry::verify_refcounts_exact(state) &&
        (SpectralTelemetry::evaluate(state) ==
         SpectralTelemetry::evaluate_full_scan(state));

    const RunSummary summary = summarize_provisioning_run(
        algorithm, records, refcounts_ok,
        orchestrator.scratchpad().theorem1_bypass_steps,
        orchestrator.scratchpad().theorem1_fc_checked_steps);

    ProvisioningRunView view{
        .algorithm = algorithm,
        .state = state,
        .records = records,
        .summary = summary,
    };
    on_result(view);
  };

  for (const AlgorithmId alg : algorithms) {
    switch (alg) {
      case AlgorithmId::SimRDF: {
        if (!sim_rdf) {
          sim_rdf = std::make_unique<SimRdfOrchestrator>(
              graph, scenarios, prewarmed_pool, max_req);
        }
        run_single(*sim_rdf, get_sim_state(), alg);
        break;
      }
      case AlgorithmId::GreedySimRDF: {
        if (!greedy_sim_rdf) {
          greedy_sim_rdf = std::make_unique<GreedySimRdfOrchestrator>(
              graph, scenarios, prewarmed_pool, max_req);
        }
        run_single(*greedy_sim_rdf, get_sim_state(), alg);
        break;
      }
      case AlgorithmId::NoRerouteSimRDF: {
        if (!no_reroute_sim_rdf) {
          no_reroute_sim_rdf = std::make_unique<NoRerouteSimRdfOrchestrator>(
              graph, scenarios, prewarmed_pool, max_req);
        }
        run_single(*no_reroute_sim_rdf, get_sim_state(), alg);
        break;
      }
      case AlgorithmId::SeqRDF: {
        if (!seq_rdf) {
          seq_rdf = std::make_unique<SeqRdfOrchestrator>(
              graph, scenarios, prewarmed_pool, max_req);
        }
        run_single(*seq_rdf, get_seq_state(), alg);
        break;
      }
      case AlgorithmId::GreedySeqRDF: {
        if (!greedy_seq_rdf) {
          greedy_seq_rdf = std::make_unique<GreedySeqRdfOrchestrator>(
              graph, scenarios, prewarmed_pool, max_req);
        }
        run_single(*greedy_seq_rdf, get_seq_state(), alg);
        break;
      }
      case AlgorithmId::NoRerouteSeqRDF: {
        if (!no_reroute_seq_rdf) {
          no_reroute_seq_rdf = std::make_unique<NoRerouteSeqRdfOrchestrator>(
              graph, scenarios, prewarmed_pool, max_req);
        }
        run_single(*no_reroute_seq_rdf, get_seq_state(), alg);
        break;
      }
    }
  }
}

void append_run_summary_csv(const std::filesystem::path& csv_path,
                            std::string_view exp, std::string_view topology,
                            std::string_view cut_model,
                            std::string_view traffic, std::string_view seq_id,
                            const RunSummary& summary,
                            bool invariants_verified) {
  SpectralTelemetry::append_summary_csv_row(
      csv_path, exp, topology, cut_model, traffic, seq_id, summary.algorithm,
      summary.regime, algorithm_heuristic_name(summary.algorithm),
      summary.first_denial_idx, summary.total_granted, summary.blocking_prob,
      summary.mean_latency_pre_denial_us, summary.p95_latency_pre_denial_us,
      summary.mean_latency_full_us, summary.mean_latency_rejected_us,
      summary.spectral_at_denial.u_prim_pct,
      summary.spectral_at_denial.u_pure_pct,
      summary.spectral_at_denial.overhead_ratio,
      summary.spectral_final.u_prim_pct, summary.spectral_final.u_pure_pct,
      summary.spectral_final.u_stub_pct, summary.spectral_final.overhead_ratio,
      invariants_verified);
}

void write_telemetry_json(const std::filesystem::path& json_path,
                          std::string_view exp, std::string_view topology,
                          std::string_view cut_model, std::string_view traffic,
                          std::string_view seq_id,
                          std::span<const RunSummary> summaries,
                          std::span<const uint8_t> invariants_verified) {
  if (json_path.has_parent_path()) {
    std::filesystem::create_directories(json_path.parent_path());
  }
  std::filesystem::path tmp_path = json_path;
  tmp_path += ".tmp";

  {
    std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
      throw std::runtime_error("Failed to open telemetry JSON for writing: " +
                               tmp_path.string());
    }
    ofs << "{\n"
        << "  \"exp\": \"" << exp << "\",\n"
        << "  \"topology\": \"" << topology << "\",\n"
        << "  \"cut_model\": \"" << cut_model << "\",\n"
        << "  \"traffic\": \"" << traffic << "\",\n"
        << "  \"seq_id\": \"" << seq_id << "\",\n"
        << "  \"runs\": [\n";

    for (size_t i = 0; i < summaries.size(); ++i) {
      const RunSummary& s = summaries[i];
      const bool inv =
          (i < invariants_verified.size()) ? invariants_verified[i] : false;
      ofs << "    {\n"
          << "      \"algorithm\": \"" << algorithm_name(s.algorithm) << "\",\n"
          << "      \"regime\": \""
          << (s.regime == FailureRegime::Simultaneous ? "Simultaneous"
                                                      : "Sequential")
          << "\",\n"
          << "      \"heuristic\": \"" << algorithm_heuristic_name(s.algorithm)
          << "\",\n"
          << "      \"first_denial_idx\": " << s.first_denial_idx << ",\n"
          << "      \"total_granted\": " << s.total_granted << ",\n"
          << std::fixed << std::setprecision(6)
          << "      \"blocking_prob\": " << s.blocking_prob << ",\n"
          << "      \"mean_latency_pre_denial_us\": "
          << s.mean_latency_pre_denial_us << ",\n"
          << "      \"p95_latency_pre_denial_us\": "
          << s.p95_latency_pre_denial_us << ",\n"
          << "      \"mean_latency_full_us\": " << s.mean_latency_full_us
          << ",\n"
          << "      \"mean_latency_rejected_us\": "
          << s.mean_latency_rejected_us << ",\n"
          << "      \"u_prim_at_denial\": " << s.spectral_at_denial.u_prim_pct
          << ",\n"
          << "      \"u_pure_at_denial\": " << s.spectral_at_denial.u_pure_pct
          << ",\n"
          << "      \"overhead_at_denial\": "
          << s.spectral_at_denial.overhead_ratio << ",\n"
          << "      \"u_prim_final\": " << s.spectral_final.u_prim_pct << ",\n"
          << "      \"u_pure_final\": " << s.spectral_final.u_pure_pct << ",\n"
          << "      \"u_stub_final\": " << s.spectral_final.u_stub_pct << ",\n"
          << "      \"overhead_final\": " << s.spectral_final.overhead_ratio
          << ",\n"
          << "      \"refcounts_exact\": "
          << (s.refcounts_exact ? "true" : "false") << ",\n"
          << "      \"dsatur_theorem1_bypass_steps\": "
          << s.dsatur_theorem1_bypass_steps << ",\n"
          << "      \"dsatur_theorem1_fc_checked_steps\": "
          << s.dsatur_theorem1_fc_checked_steps << ",\n"
          << "      \"invariants_verified\": " << (inv ? "true" : "false")
          << "\n"
          << "    }" << (i + 1 < summaries.size() ? "," : "") << "\n";
    }
    ofs << "  ]\n}\n";
    ofs.flush();
    if (!ofs.good()) {
      throw std::runtime_error("I/O error writing telemetry JSON: " +
                               tmp_path.string());
    }
  }
  std::filesystem::rename(tmp_path, json_path);
}

}  // namespace ton
