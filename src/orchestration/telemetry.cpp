#include "ton/orchestration/telemetry.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace ton {

namespace {

[[nodiscard]] SpectralMetrics finalize_metrics(uint32_t total_slots,
                                               uint32_t h_prim, uint32_t h_back,
                                               uint32_t h_pure,
                                               uint32_t h_stub) noexcept {
  if (total_slots == 0u) {
    return SpectralMetrics{};
  }
  const double inv_total_pct = 100.0 / static_cast<double>(total_slots);
  return SpectralMetrics{
      .h_prim_slots = h_prim,
      .h_back_slots = h_back,
      .h_pure_slots = h_pure,
      .h_stub_slots = h_stub,
      .u_prim_pct = static_cast<double>(h_prim) * inv_total_pct,
      .u_back_pct = static_cast<double>(h_back) * inv_total_pct,
      .u_pure_pct = static_cast<double>(h_pure) * inv_total_pct,
      .u_stub_pct = static_cast<double>(h_stub) * inv_total_pct,
      .u_total_pct = static_cast<double>(h_prim + h_pure) * inv_total_pct,
      .overhead_ratio = (h_prim > 0u) ? (static_cast<double>(h_pure) /
                                         static_cast<double>(h_prim))
                                      : 0.0,
  };
}

}  // namespace

SpectralMetrics SpectralTelemetry::evaluate(
    const NetworkState& state) noexcept {
  const ArcId m = state.num_arcs();
  const uint32_t total_slots = static_cast<uint32_t>(m) * NUM_WAVELENGTHS;
  if (total_slots == 0u) {
    return SpectralMetrics{};
  }

  const auto prim_masks = state.primary_masks();
  const auto back_masks = state.backup_active_masks();

  uint32_t h_prim = 0;
  uint32_t h_back = 0;
  uint32_t h_pure = 0;
  uint32_t h_stub = 0;

  for (ArcId a = 0; a < m; ++a) {
    const WavelengthMask pm = prim_masks[a];
    const WavelengthMask bm = back_masks[a];
    h_prim += static_cast<uint32_t>(std::popcount(pm));
    h_back += static_cast<uint32_t>(std::popcount(bm));
    h_pure += static_cast<uint32_t>(std::popcount(bm & ~pm));
    h_stub += static_cast<uint32_t>(std::popcount(bm & pm));
  }

  return finalize_metrics(total_slots, h_prim, h_back, h_pure, h_stub);
}

SpectralMetrics SpectralTelemetry::evaluate_full_scan(
    const NetworkState& state) noexcept {
  const ArcId m = state.num_arcs();
  const uint32_t total_slots = static_cast<uint32_t>(m) * NUM_WAVELENGTHS;
  if (total_slots == 0u) {
    return SpectralMetrics{};
  }

  uint32_t h_prim = 0;
  uint32_t h_back = 0;
  uint32_t h_pure = 0;
  uint32_t h_stub = 0;

  for (ArcId a = 0; a < m; ++a) {
    const WavelengthMask pm = state.primary_mask(a);
    WavelengthMask bm = EMPTY_MASK;

    if (state.regime() == FailureRegime::Simultaneous) {
      const uint32_t num_sim = state.num_domain_states(StateDomain::Sim);
      for (uint32_t f = 0; f < num_sim; ++f) {
        bm |= state.sim_backup_mask(f, a);
      }
    } else {
      const uint32_t num_s1 = state.num_domain_states(StateDomain::SeqStage1);
      for (uint32_t x1 = 0; x1 < num_s1; ++x1) {
        bm |= state.seq_stage1_backup_mask(static_cast<FailureUnitId>(x1), a);
      }
      const uint32_t num_s2 = state.num_domain_states(StateDomain::SeqStage2);
      for (uint32_t f = 0; f < num_s2; ++f) {
        bm |= state.seq_stage2_backup_mask(f, a);
      }
    }

    h_prim += static_cast<uint32_t>(std::popcount(pm));
    h_back += static_cast<uint32_t>(std::popcount(bm));
    h_pure += static_cast<uint32_t>(std::popcount(bm & ~pm));
    h_stub += static_cast<uint32_t>(std::popcount(bm & pm));
  }

  return finalize_metrics(total_slots, h_prim, h_back, h_pure, h_stub);
}

bool SpectralTelemetry::verify_refcounts_exact(
    const NetworkState& state) noexcept {
  const ArcId m = state.num_arcs();
  for (ArcId a = 0; a < m; ++a) {
    std::array<uint32_t, NUM_WAVELENGTHS> exact_counts{};
    if (state.regime() == FailureRegime::Simultaneous) {
      const uint32_t num_sim = state.num_domain_states(StateDomain::Sim);
      for (uint32_t f = 0; f < num_sim; ++f) {
        WavelengthMask sm = state.sim_backup_mask(f, a);
        while (sm != EMPTY_MASK) {
          const uint8_t w = lowest_wavelength(sm);
          sm &= (sm - 1u);
          ++exact_counts[w];
        }
      }
    } else {
      const uint32_t num_s1 = state.num_domain_states(StateDomain::SeqStage1);
      for (uint32_t x1 = 0; x1 < num_s1; ++x1) {
        WavelengthMask sm =
            state.seq_stage1_backup_mask(static_cast<FailureUnitId>(x1), a);
        while (sm != EMPTY_MASK) {
          const uint8_t w = lowest_wavelength(sm);
          sm &= (sm - 1u);
          ++exact_counts[w];
        }
      }
      const uint32_t num_s2 = state.num_domain_states(StateDomain::SeqStage2);
      for (uint32_t f = 0; f < num_s2; ++f) {
        WavelengthMask sm = state.seq_stage2_backup_mask(f, a);
        while (sm != EMPTY_MASK) {
          const uint8_t w = lowest_wavelength(sm);
          sm &= (sm - 1u);
          ++exact_counts[w];
        }
      }
    }

    WavelengthMask expected_active = EMPTY_MASK;
    for (uint8_t w = 0; w < NUM_WAVELENGTHS; ++w) {
      if (exact_counts[w] != state.backup_slot_refcount(a, w)) {
        return false;
      }
      if (exact_counts[w] > 0u) {
        expected_active |= wavelength_bit(w);
      }
    }
    if (expected_active != state.backup_active_mask(a)) {
      return false;
    }
  }
  return true;
}

void SpectralTelemetry::write_csv_header(std::ostream& os) {
  os << "t,src,dst,admitted,latency_us,k_adm,h_prim_slots,h_back_slots,"
        "h_pure_slots,h_stub_slots,u_prim_pct,u_back_pct,u_pure_pct,"
        "u_stub_pct,u_total_pct,overhead_ratio\n";
}

void SpectralTelemetry::write_csv_row(std::ostream& os,
                                      const StepRecord& record) {
  const auto old_flags = os.flags();
  const auto old_prec = os.precision();
  os << record.t << ',' << record.src << ',' << record.dst << ','
     << (record.admitted ? 1 : 0) << ',' << std::fixed << std::setprecision(4)
     << record.latency_us << ',' << record.k_adm << ','
     << record.spectral.h_prim_slots << ',' << record.spectral.h_back_slots
     << ',' << record.spectral.h_pure_slots << ','
     << record.spectral.h_stub_slots << ',' << std::setprecision(6)
     << record.spectral.u_prim_pct << ',' << record.spectral.u_back_pct << ','
     << record.spectral.u_pure_pct << ',' << record.spectral.u_stub_pct << ','
     << record.spectral.u_total_pct << ',' << record.spectral.overhead_ratio
     << '\n';
  os.flags(old_flags);
  os.precision(old_prec);
}

void SpectralTelemetry::write_step_records_csv(
    const std::filesystem::path& csv_path,
    std::span<const StepRecord> records) {
  if (csv_path.has_parent_path()) {
    std::filesystem::create_directories(csv_path.parent_path());
  }
  std::filesystem::path tmp_path = csv_path;
  tmp_path += ".tmp";

  {
    std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
      throw std::runtime_error("Failed to open telemetry CSV for writing: " +
                               tmp_path.string());
    }
    write_csv_header(ofs);
    for (const StepRecord& rec : records) {
      write_csv_row(ofs, rec);
    }
    ofs.flush();
    if (!ofs.good()) {
      throw std::runtime_error("I/O error writing telemetry CSV: " +
                               tmp_path.string());
    }
  }
  std::filesystem::rename(tmp_path, csv_path);
}

void SpectralTelemetry::write_summary_csv_header(std::ostream& os) {
  os << MASTER_SUMMARY_CSV_HEADER;
}

void SpectralTelemetry::write_summary_csv_row(
    std::ostream& os, std::string_view exp, std::string_view topology,
    std::string_view cut_model, std::string_view traffic,
    std::string_view seq_id, AlgorithmId algorithm, FailureRegime regime,
    std::string_view heuristic, uint32_t first_denial_idx,
    uint32_t total_granted, double blocking_prob,
    double mean_latency_pre_denial_us, double p95_latency_pre_denial_us,
    double mean_latency_full_us, double mean_latency_rejected_us,
    double u_prim_at_denial, double u_pure_at_denial, double overhead_at_denial,
    double u_prim_final, double u_pure_final, double u_stub_final,
    double overhead_final, bool invariants_verified) {
  const auto old_flags = os.flags();
  const auto old_prec = os.precision();
  os << exp << ',' << topology << ',' << cut_model << ',' << traffic << ','
     << seq_id << ',' << algorithm_name(algorithm) << ','
     << (regime == FailureRegime::Simultaneous ? "Simultaneous" : "Sequential")
     << ',' << heuristic << ',' << first_denial_idx << ',' << total_granted
     << ',' << std::fixed << std::setprecision(6) << blocking_prob << ','
     << mean_latency_pre_denial_us << ',' << p95_latency_pre_denial_us << ','
     << mean_latency_full_us << ',' << mean_latency_rejected_us << ','
     << u_prim_at_denial << ',' << u_pure_at_denial << ',' << overhead_at_denial
     << ',' << u_prim_final << ',' << u_pure_final << ',' << u_stub_final << ','
     << overhead_final << ',' << (invariants_verified ? "true" : "false")
     << '\n';
  os.flags(old_flags);
  os.precision(old_prec);
}

std::string SpectralTelemetry::format_summary_csv_row(
    std::string_view exp, std::string_view topology, std::string_view cut_model,
    std::string_view traffic, std::string_view seq_id, AlgorithmId algorithm,
    FailureRegime regime, std::string_view heuristic, uint32_t first_denial_idx,
    uint32_t total_granted, double blocking_prob,
    double mean_latency_pre_denial_us, double p95_latency_pre_denial_us,
    double mean_latency_full_us, double mean_latency_rejected_us,
    double u_prim_at_denial, double u_pure_at_denial, double overhead_at_denial,
    double u_prim_final, double u_pure_final, double u_stub_final,
    double overhead_final, bool invariants_verified) {
  std::ostringstream oss;
  write_summary_csv_row(
      oss, exp, topology, cut_model, traffic, seq_id, algorithm, regime,
      heuristic, first_denial_idx, total_granted, blocking_prob,
      mean_latency_pre_denial_us, p95_latency_pre_denial_us,
      mean_latency_full_us, mean_latency_rejected_us, u_prim_at_denial,
      u_pure_at_denial, overhead_at_denial, u_prim_final, u_pure_final,
      u_stub_final, overhead_final, invariants_verified);
  return oss.str();
}

void SpectralTelemetry::append_summary_csv_row(
    const std::filesystem::path& csv_path, std::string_view exp,
    std::string_view topology, std::string_view cut_model,
    std::string_view traffic, std::string_view seq_id, AlgorithmId algorithm,
    FailureRegime regime, std::string_view heuristic, uint32_t first_denial_idx,
    uint32_t total_granted, double blocking_prob,
    double mean_latency_pre_denial_us, double p95_latency_pre_denial_us,
    double mean_latency_full_us, double mean_latency_rejected_us,
    double u_prim_at_denial, double u_pure_at_denial, double overhead_at_denial,
    double u_prim_final, double u_pure_final, double u_stub_final,
    double overhead_final, bool invariants_verified) {
  if (csv_path.has_parent_path()) {
    std::filesystem::create_directories(csv_path.parent_path());
  }

  const std::string row_str = format_summary_csv_row(
      exp, topology, cut_model, traffic, seq_id, algorithm, regime, heuristic,
      first_denial_idx, total_granted, blocking_prob,
      mean_latency_pre_denial_us, p95_latency_pre_denial_us,
      mean_latency_full_us, mean_latency_rejected_us, u_prim_at_denial,
      u_pure_at_denial, overhead_at_denial, u_prim_final, u_pure_final,
      u_stub_final, overhead_final, invariants_verified);

  const int fd = ::open(csv_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    throw std::runtime_error("Failed to open summary CSV for appending: " +
                             csv_path.string());
  }

  if (::flock(fd, LOCK_EX) != 0) {
    ::close(fd);
    throw std::runtime_error("Failed to acquire flock on summary CSV: " +
                             csv_path.string());
  }

  struct stat st{};
  if (::fstat(fd, &st) == 0 && st.st_size == 0) {
    const std::string_view header = MASTER_SUMMARY_CSV_HEADER;
    const ssize_t hw = ::write(fd, header.data(), header.size());
    if (hw != static_cast<ssize_t>(header.size())) {
      ::flock(fd, LOCK_UN);
      ::close(fd);
      throw std::runtime_error("Failed to write header to summary CSV: " +
                               csv_path.string());
    }
  }

  const ssize_t rw = ::write(fd, row_str.data(), row_str.size());
  if (rw != static_cast<ssize_t>(row_str.size())) {
    ::flock(fd, LOCK_UN);
    ::close(fd);
    throw std::runtime_error("Failed to write row to summary CSV: " +
                             csv_path.string());
  }

  ::fdatasync(fd);
  ::flock(fd, LOCK_UN);
  ::close(fd);
}

}  // namespace ton
