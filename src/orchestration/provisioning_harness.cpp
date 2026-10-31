#include "ton/orchestration/provisioning_harness.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
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

}  // namespace ton
