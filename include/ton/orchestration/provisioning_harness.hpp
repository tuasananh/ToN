#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "ton/core/assert.hpp"
#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"
#include "ton/orchestration/network_state.hpp"
#include "ton/orchestration/seq_orchestrator.hpp"
#include "ton/orchestration/sim_orchestrator.hpp"
#include "ton/orchestration/telemetry.hpp"
#include "ton/solvers/dsatur_solver.hpp"
#include "ton/solvers/greedy_solver.hpp"
#include "ton/solvers/no_reroute_solver.hpp"

namespace ton {

// Canonical 6-algorithm static template instantiations (ALGORITHMS.md Section 3
// & PHASE-3.md Section 3.5).
using SimRdfOrchestrator = SimOrchestrator<DsaturSolver>;
using SeqRdfOrchestrator = SeqOrchestrator<DsaturSolver>;
using GreedySimRdfOrchestrator = SimOrchestrator<GreedySolver>;
using GreedySeqRdfOrchestrator = SeqOrchestrator<GreedySolver>;
using NoRerouteSimRdfOrchestrator = SimOrchestrator<NoRerouteSolver>;
using NoRerouteSeqRdfOrchestrator = SeqOrchestrator<NoRerouteSolver>;

[[nodiscard]] bool try_parse_algorithm_id(std::string_view name,
                                          AlgorithmId& out_alg) noexcept;

// Offline stream CSV loader for frozen 1,000-request arrival sequences
// (`t,src,dst,src_name,dst_name`).
[[nodiscard]] std::vector<Request> load_request_stream_csv(
    const std::filesystem::path& csv_path, size_t max_count = 1000);

// Optional zero-overhead hooks around the pure online provisioning loop
// (used by test gates to arm/disarm zero-heap-allocation verifiers strictly
// around t = 0..T-1).
struct OnlineLoopHooks {
  void (*on_before_loop)() noexcept = nullptr;
  void (*on_after_loop)() noexcept = nullptr;
};

// Zero-allocation online request provisioning loop (t = 0 .. T-1).
// All working buffers in `orchestrator`, `state`, `PathPool`, and `out_records`
// must be pre-allocated/pre-warmed prior to entering this function.
template <typename Orchestrator>
void execute_online_provisioning_loop(Orchestrator& orchestrator,
                                      NetworkState& state,
                                      std::span<const Request> requests,
                                      std::span<StepRecord> out_records,
                                      OnlineLoopHooks hooks = {}) noexcept {
  TON_ASSERT(out_records.size() >= requests.size(),
             "out_records must be pre-allocated to requests.size()");
  if (hooks.on_before_loop != nullptr) {
    hooks.on_before_loop();
  }

  for (size_t idx = 0; idx < requests.size(); ++idx) {
    const Request& req = requests[idx];
    const auto t0 = std::chrono::steady_clock::now();
    const AdmissionResult res = orchestrator.admit_request(req, state);
    const auto t1 = std::chrono::steady_clock::now();
    const double latency_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count();

    out_records[idx] = StepRecord{
        .t = req.id,
        .src = req.src,
        .dst = req.dst,
        .admitted = res.admitted,
        .latency_us = latency_us,
        .k_adm = state.num_admitted(),
        .spectral = SpectralTelemetry::evaluate(state),
    };
  }

  if (hooks.on_after_loop != nullptr) {
    hooks.on_after_loop();
  }
}

// Run-level aggregate metrics matching PROBLEM.md Section 8.3 & PLAN.md 5.1.
struct RunSummary {
  AlgorithmId algorithm = AlgorithmId::SimRDF;
  FailureRegime regime = FailureRegime::Simultaneous;
  uint32_t num_requests = 0;
  uint32_t first_denial_idx = 0;
  uint32_t total_granted = 0;
  double blocking_prob = 0.0;
  double mean_latency_pre_denial_us = 0.0;
  double p95_latency_pre_denial_us = 0.0;
  double mean_latency_full_us = 0.0;
  double mean_latency_rejected_us = 0.0;
  SpectralMetrics spectral_at_denial{};
  SpectralMetrics spectral_final{};
  bool refcounts_exact = false;
  uint64_t dsatur_theorem1_bypass_steps = 0;
  uint64_t dsatur_theorem1_fc_checked_steps = 0;
};

[[nodiscard]] RunSummary summarize_provisioning_run(
    AlgorithmId algorithm, std::span<const StepRecord> records,
    bool refcounts_exact, uint64_t theorem1_bypass_steps = 0,
    uint64_t theorem1_fc_steps = 0);

void append_run_summary_csv(const std::filesystem::path& csv_path,
                            std::string_view exp, std::string_view topology,
                            std::string_view cut_model,
                            std::string_view traffic, std::string_view seq_id,
                            const RunSummary& summary,
                            bool invariants_verified);

void write_telemetry_json(const std::filesystem::path& json_path,
                          std::string_view exp, std::string_view topology,
                          std::string_view cut_model, std::string_view traffic,
                          std::string_view seq_id,
                          std::span<const RunSummary> summaries,
                          std::span<const uint8_t> invariants_verified);

struct ProvisioningRunResult {
  NetworkState state;
  std::vector<StepRecord> records;
  RunSummary summary;
};

// Executes one of the 6 canonical algorithms to completion over `requests`
// using a pre-warmed `path_pool`, returning the final NetworkState, all T
// step-indexed records, and the RunSummary (including bit-for-bit verification
// of backup_slot_refcount against full state scans).
[[nodiscard]] ProvisioningRunResult run_provisioning_algorithm(
    AlgorithmId algorithm, const Graph& graph, const ScenarioSet& scenarios,
    const PathPool& prewarmed_pool, std::span<const Request> requests,
    OnlineLoopHooks hooks = {});

}  // namespace ton
