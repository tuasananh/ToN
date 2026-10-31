#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ton/core/graph.hpp"
#include "ton/core/path_pool.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"
#include "ton/orchestration/provisioning_harness.hpp"
#include "ton/orchestration/telemetry.hpp"

namespace {

void print_usage(std::ostream& os, std::string_view prog) {
  os << "Usage: " << prog << " [options]\n"
     << "Options:\n"
     << "  --topology <xml>       Path to SNDlib topology XML\n"
     << "  --stream <csv>         Path to frozen arrival stream CSV\n"
     << "  --cut-model <m>        span (default) or directional\n"
     << "  --algorithm <alg>      SimRDF | SeqRDF | GreedySimRDF | "
        "GreedySeqRDF |\n"
     << "                         NoRerouteSimRDF | NoRerouteSeqRDF | all "
        "(default)\n"
     << "  --max-requests <N>     Max requests to process (default: 1000)\n"
     << "  --output-steps <csv>   Write per-step trajectory CSV (single alg)\n"
     << "  --output-dir <dir>     Write <algorithm>_steps.csv into directory\n"
     << "  -h, --help             Show this help message\n";
}

void write_steps_atomic(const std::filesystem::path& out_path,
                        std::span<const ton::StepRecord> records) {
  ton::SpectralTelemetry::write_step_records_csv(out_path, records);
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << "ToN 2026 Optical Survivability Engine (W="
            << static_cast<int>(ton::NUM_WAVELENGTHS) << ")\n";
  if (argc <= 1) {
    return EXIT_SUCCESS;
  }

  std::filesystem::path topology_path;
  std::filesystem::path stream_path;
  std::filesystem::path output_steps_path;
  std::filesystem::path output_dir_path;
  ton::CutModel cut_model = ton::CutModel::Span;
  bool run_all_algorithms = true;
  ton::AlgorithmId single_algorithm = ton::AlgorithmId::SimRDF;
  size_t max_requests = 1000;

  try {
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      auto require_value = [&](std::string_view flag) -> std::string_view {
        if (i + 1 >= argc) {
          throw std::invalid_argument("Missing argument for " +
                                      std::string(flag));
        }
        return std::string_view(argv[++i]);
      };

      if (arg == "-h" || arg == "--help") {
        print_usage(std::cout, argv[0]);
        return EXIT_SUCCESS;
      }
      if (arg == "--topology") {
        topology_path = require_value(arg);
      } else if (arg == "--stream") {
        stream_path = require_value(arg);
      } else if (arg == "--cut-model") {
        const std::string_view val = require_value(arg);
        if (val == "span" || val == "Span") {
          cut_model = ton::CutModel::Span;
        } else if (val == "directional" || val == "Directional") {
          cut_model = ton::CutModel::Directional;
        } else {
          throw std::invalid_argument("Invalid --cut-model: " +
                                      std::string(val));
        }
      } else if (arg == "--algorithm") {
        const std::string_view val = require_value(arg);
        if (val == "all" || val == "ALL") {
          run_all_algorithms = true;
        } else if (ton::try_parse_algorithm_id(val, single_algorithm)) {
          run_all_algorithms = false;
        } else {
          throw std::invalid_argument("Invalid --algorithm: " +
                                      std::string(val));
        }
      } else if (arg == "--max-requests") {
        max_requests = std::stoull(std::string(require_value(arg)));
      } else if (arg == "--output-steps") {
        output_steps_path = require_value(arg);
      } else if (arg == "--output-dir") {
        output_dir_path = require_value(arg);
      } else {
        throw std::invalid_argument("Unknown option: " + std::string(arg));
      }
    }

    if (topology_path.empty() || stream_path.empty()) {
      throw std::invalid_argument(
          "Both --topology <xml> and --stream <csv> are required");
    }
    if (!output_steps_path.empty() && run_all_algorithms) {
      throw std::invalid_argument(
          "--output-steps requires a single --algorithm (use --output-dir for "
          "--algorithm all)");
    }

    const ton::Graph graph = ton::Graph::from_sndlib_xml(topology_path);
    const bool is_canonical_bench =
        (graph.name() == "nobel-eu" || graph.name() == "germany50");
    const ton::ScenarioSet scenarios =
        ton::ScenarioSet::enumerate(graph, cut_model, is_canonical_bench);
    const std::vector<ton::Request> requests =
        ton::load_request_stream_csv(stream_path, max_requests);

    const auto pw_t0 = std::chrono::steady_clock::now();
    ton::PathPool path_pool(graph, ton::DistanceMetric::GeographicKm,
                            ton::DEFAULT_KAPPA, ton::DEFAULT_K_MAX);
    path_pool.prewarm_for_stream(requests, scenarios);
    const auto pw_t1 = std::chrono::steady_clock::now();
    const double prewarm_ms =
        std::chrono::duration<double, std::milli>(pw_t1 - pw_t0).count();

    std::cout << std::fixed << std::setprecision(2)
              << "Topology: " << graph.name() << " (" << graph.num_nodes()
              << "N, " << graph.num_spans() << "E, " << graph.num_arcs()
              << "L) | CutModel: "
              << (cut_model == ton::CutModel::Span ? "Span" : "Directional")
              << " (" << scenarios.sim_scenarios().size() << " Sim / "
              << scenarios.seq_scenarios().size() << " Seq)"
              << " | Stream: " << requests.size() << " reqs"
              << " | PathPool: " << path_pool.num_paths() << " paths ("
              << prewarm_ms << " ms)\n";

    std::vector<ton::AlgorithmId> selected;
    if (run_all_algorithms) {
      selected.assign(ton::ALL_ALGORITHMS.begin(), ton::ALL_ALGORITHMS.end());
    } else {
      selected.push_back(single_algorithm);
    }

    std::cout << std::left << std::setw(17) << "Algorithm" << std::right
              << std::setw(10) << "I_denial" << std::setw(11) << "K_granted"
              << std::setw(9) << "Block%" << std::setw(13) << "T_pre(us)"
              << std::setw(13) << "T_full(us)" << std::setw(11) << "U_prim%"
              << std::setw(11) << "U_pure%" << std::setw(11) << "U_stub%"
              << std::setw(11) << "Overhead" << std::setw(10) << "RefcntOK\n";

    for (const ton::AlgorithmId alg : selected) {
      const ton::ProvisioningRunResult run = ton::run_provisioning_algorithm(
          alg, graph, scenarios, path_pool, requests);
      const ton::RunSummary& s = run.summary;

      std::cout << std::left << std::setw(17) << ton::algorithm_name(alg)
                << std::right << std::setw(10) << s.first_denial_idx
                << std::setw(11) << s.total_granted << std::setw(9)
                << std::setprecision(2) << (s.blocking_prob * 100.0)
                << std::setw(13) << std::setprecision(2)
                << s.mean_latency_pre_denial_us << std::setw(13)
                << s.mean_latency_full_us << std::setw(11)
                << s.spectral_final.u_prim_pct << std::setw(11)
                << s.spectral_final.u_pure_pct << std::setw(11)
                << s.spectral_final.u_stub_pct << std::setw(11)
                << std::setprecision(4) << s.spectral_final.overhead_ratio
                << std::setw(10) << (s.refcounts_exact ? "true" : "FALSE")
                << "\n";

      if (!s.refcounts_exact) {
        std::cerr << "[FATAL] backup_slot_refcount mismatch for "
                  << ton::algorithm_name(alg) << "\n";
        return EXIT_FAILURE;
      }

      if (!output_steps_path.empty()) {
        write_steps_atomic(output_steps_path, run.records);
      }
      if (!output_dir_path.empty()) {
        const std::filesystem::path p =
            output_dir_path /
            (std::string(ton::algorithm_name(alg)) + "_steps.csv");
        write_steps_atomic(p, run.records);
      }
    }
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "[ERROR] " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
