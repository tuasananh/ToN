#include "test_helpers.hpp"

#include <cstdlib>
#include <new>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TON_ASAN_ACTIVE 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define TON_ASAN_ACTIVE 1
#endif

namespace ton::test {

#if !defined(TON_ASAN_ACTIVE)
static thread_local bool g_track_allocations = false;
static thread_local uint64_t g_allocation_count = 0;
#endif

bool is_asan_active() noexcept {
#if defined(TON_ASAN_ACTIVE)
  return true;
#else
  return false;
#endif
}

void arm_zero_alloc_monitor() noexcept {
#if !defined(TON_ASAN_ACTIVE)
  g_allocation_count = 0;
  g_track_allocations = true;
#endif
}

void disarm_zero_alloc_monitor() noexcept {
#if !defined(TON_ASAN_ACTIVE)
  g_track_allocations = false;
#endif
}

uint64_t tracked_allocations() noexcept {
#if !defined(TON_ASAN_ACTIVE)
  return g_allocation_count;
#else
  return 0;
#endif
}

void audit_simultaneous_state_invariants(const NetworkState& state,
                                         const PathPool& pool) {
  REQUIRE(state.regime() == FailureRegime::Simultaneous);
  const auto& g = state.graph();
  const auto& sc_set = state.scenarios();
  const ArcId m = g.num_arcs();

  // INV-1 & INV-4: Primary clash conservation & route admissibility.
  std::vector<WavelengthMask> prim_occ(m, EMPTY_MASK);
  for (const RequestId r : state.admitted_ids()) {
    REQUIRE(state.is_admitted(r));
    const Request& req = state.request(r);
    const Lightpath lp = state.primary_lightpath(r);
    REQUIRE(lp.valid());

    const PathSlice cand = pool.find_healthy(req.src, req.dst);
    bool in_pool = false;
    for (const PathId pid : cand) {
      if (pid == lp.path_id) {
        in_pool = true;
        break;
      }
    }
    REQUIRE(in_pool);

    const WavelengthMask bit = wavelength_bit(lp.wavelength);
    for (const ArcId a : pool.path_arcs(lp.path_id)) {
      REQUIRE((prim_occ[a] & bit) == 0u);
      REQUIRE(state.primary_owner(a, lp.wavelength) == r);
      prim_occ[a] |= bit;
    }
  }
  for (ArcId a = 0; a < m; ++a) {
    REQUIRE(prim_occ[a] == state.primary_mask(a));
  }

  // INV-2, INV-3, INV-4, INV-5 across all Simultaneous scenarios F in F_sim.
  std::vector<WavelengthMask> scen_occ(m, EMPTY_MASK);
  std::vector<WavelengthMask> back_only(m, EMPTY_MASK);
  for (const auto& sc : sc_set.sim_scenarios()) {
    std::fill(scen_occ.begin(), scen_occ.end(), EMPTY_MASK);
    std::fill(back_only.begin(), back_only.end(), EMPTY_MASK);

    uint32_t expected_disrupted = 0;
    for (const RequestId r : state.admitted_ids()) {
      const Lightpath prim = state.primary_lightpath(r);
      const bool severed = pool.path_intersects(prim.path_id, sc.cut_arcs);
      if (!severed) {
        REQUIRE_FALSE(state.find_sim_backup(sc.id, r).valid());
        const WavelengthMask bit = wavelength_bit(prim.wavelength);
        for (const ArcId a : pool.path_arcs(prim.path_id)) {
          scen_occ[a] |= bit;
        }
      } else {
        ++expected_disrupted;
      }
    }

    const auto backups = state.sim_backups(sc.id);
    REQUIRE(backups.size() == expected_disrupted);

    for (const auto& ba : backups) {
      REQUIRE(ba.valid());
      REQUIRE(state.is_admitted(ba.req_id));
      const Request& req = state.request(ba.req_id);
      const Lightpath prim = state.primary_lightpath(ba.req_id);
      REQUIRE(pool.path_intersects(prim.path_id, sc.cut_arcs));
      REQUIRE_FALSE(pool.path_intersects(ba.path_id, sc.cut_arcs));

      const PathSlice cand = pool.find(sc.cut_arcs, req.src, req.dst);
      bool in_pool = false;
      for (const PathId pid : cand) {
        if (pid == ba.path_id) {
          in_pool = true;
          break;
        }
      }
      REQUIRE(in_pool);

      const WavelengthMask bit = wavelength_bit(ba.wavelength);
      for (const ArcId a : pool.path_arcs(ba.path_id)) {
        REQUIRE((scen_occ[a] & bit) == 0u);
        scen_occ[a] |= bit;
        back_only[a] |= bit;
      }
    }

    for (ArcId a = 0; a < m; ++a) {
      REQUIRE(back_only[a] == state.sim_backup_mask(sc.id, a));
    }
  }

  REQUIRE(SpectralTelemetry::verify_refcounts_exact(state));
  REQUIRE(SpectralTelemetry::evaluate(state) ==
          SpectralTelemetry::evaluate_full_scan(state));
}

void audit_sequential_state_invariants(const NetworkState& state,
                                       const PathPool& pool) {
  REQUIRE(state.regime() == FailureRegime::Sequential);
  const auto& g = state.graph();
  const auto& sc_set = state.scenarios();
  const ArcId m = g.num_arcs();

  // INV-1: Primary Clash Conservation & healthy (kappa, K_max) admissibility.
  std::vector<WavelengthMask> prim_occ(m, EMPTY_MASK);
  for (const RequestId r : state.admitted_ids()) {
    REQUIRE(state.is_admitted(r));
    const Request& req = state.request(r);
    const Lightpath lp = state.primary_lightpath(r);
    REQUIRE(lp.valid());

    const PathSlice cand = pool.find_healthy(req.src, req.dst);
    bool in_pool = false;
    for (const PathId pid : cand) {
      if (pid == lp.path_id) {
        in_pool = true;
        break;
      }
    }
    REQUIRE(in_pool);

    const WavelengthMask bit = wavelength_bit(lp.wavelength);
    for (const ArcId a : pool.path_arcs(lp.path_id)) {
      REQUIRE((prim_occ[a] & bit) == 0u);
      REQUIRE(state.primary_owner(a, lp.wavelength) == r);
      prim_occ[a] |= bit;
    }
  }
  for (ArcId a = 0; a < m; ++a) {
    REQUIRE(prim_occ[a] == state.primary_mask(a));
  }

  for (FailureUnitId u = 0; u < sc_set.num_units(); ++u) {
    if (!sc_set.is_active_stage1_unit(u)) {
      REQUIRE(state.seq_stage1_backups(u).empty());
      for (ArcId a = 0; a < m; ++a) {
        REQUIRE(state.seq_stage1_backup_mask(u, a) == EMPTY_MASK);
      }
    }
  }

  std::vector<WavelengthMask> scen_occ(m, EMPTY_MASK);
  std::vector<WavelengthMask> back_only(m, EMPTY_MASK);
  std::vector<Lightpath> s1_lp_by_req(state.max_requests(), Lightpath{});
  size_t audited_s2_count = 0;

  for (const FailureUnitId x1 : sc_set.active_stage1_units()) {
    const CutArcs& s1_cut = sc_set.unit_cut_arcs(x1);
    std::fill(scen_occ.begin(), scen_occ.end(), EMPTY_MASK);
    std::fill(back_only.begin(), back_only.end(), EMPTY_MASK);
    std::fill(s1_lp_by_req.begin(), s1_lp_by_req.end(), Lightpath{});

    uint32_t expected_s1_disrupted = 0;
    for (const RequestId r : state.admitted_ids()) {
      const Lightpath prim = state.primary_lightpath(r);
      if (!pool.path_intersects(prim.path_id, s1_cut)) {
        REQUIRE_FALSE(state.find_seq_stage1_backup(x1, r).valid());
        const WavelengthMask bit = wavelength_bit(prim.wavelength);
        for (const ArcId a : pool.path_arcs(prim.path_id)) {
          scen_occ[a] |= bit;
        }
      } else {
        ++expected_s1_disrupted;
      }
    }

    const auto s1_backups = state.seq_stage1_backups(x1);
    REQUIRE(s1_backups.size() == expected_s1_disrupted);

    for (const auto& ba : s1_backups) {
      REQUIRE(ba.valid());
      REQUIRE(state.is_admitted(ba.req_id));
      REQUIRE_FALSE(s1_lp_by_req[ba.req_id].valid());
      s1_lp_by_req[ba.req_id] = ba.lightpath();

      const Request& req = state.request(ba.req_id);
      const Lightpath prim = state.primary_lightpath(ba.req_id);
      REQUIRE(pool.path_intersects(prim.path_id, s1_cut));
      REQUIRE_FALSE(pool.path_intersects(ba.path_id, s1_cut));

      const PathSlice cand = pool.find(s1_cut, req.src, req.dst);
      bool in_pool = false;
      for (const PathId pid : cand) {
        if (pid == ba.path_id) {
          in_pool = true;
          break;
        }
      }
      REQUIRE(in_pool);

      const WavelengthMask bit = wavelength_bit(ba.wavelength);
      for (const ArcId a : pool.path_arcs(ba.path_id)) {
        REQUIRE((scen_occ[a] & bit) == 0u);
        scen_occ[a] |= bit;
        back_only[a] |= bit;
      }
    }

    for (ArcId a = 0; a < m; ++a) {
      REQUIRE(back_only[a] == state.seq_stage1_backup_mask(x1, a));
    }

    for (const auto& sc : sc_set.seq_group(x1)) {
      REQUIRE(sc.x1 == x1);
      ++audited_s2_count;

      std::fill(scen_occ.begin(), scen_occ.end(), EMPTY_MASK);
      std::fill(back_only.begin(), back_only.end(), EMPTY_MASK);

      uint32_t expected_s2_disrupted = 0;
      for (const RequestId r : state.admitted_ids()) {
        const Lightpath prim = state.primary_lightpath(r);
        if (!pool.path_intersects(prim.path_id, sc.total_cut)) {
          REQUIRE_FALSE(state.find_seq_stage2_backup(sc.id, r).valid());
          const WavelengthMask bit = wavelength_bit(prim.wavelength);
          for (const ArcId a : pool.path_arcs(prim.path_id)) {
            scen_occ[a] |= bit;
          }
        } else {
          ++expected_s2_disrupted;
        }
      }

      const auto s2_backups = state.seq_stage2_backups(sc.id);
      REQUIRE(s2_backups.size() == expected_s2_disrupted);

      for (const auto& ba : s2_backups) {
        REQUIRE(ba.valid());
        REQUIRE(state.is_admitted(ba.req_id));
        const Request& req = state.request(ba.req_id);
        const Lightpath prim = state.primary_lightpath(ba.req_id);
        REQUIRE(pool.path_intersects(prim.path_id, sc.total_cut));
        REQUIRE_FALSE(pool.path_intersects(ba.path_id, sc.total_cut));

        bool locked_from_stage1 = false;
        if (pool.path_intersects(prim.path_id, sc.stage1_cut)) {
          const Lightpath s1_lp = s1_lp_by_req[ba.req_id];
          REQUIRE(s1_lp.valid());
          if (!pool.path_intersects(s1_lp.path_id, sc.stage2_cut)) {
            REQUIRE(ba.lightpath() == s1_lp);
            locked_from_stage1 = true;
          }
        } else {
          REQUIRE(pool.path_intersects(prim.path_id, sc.stage2_cut));
        }

        const PathSlice cand = pool.find(sc.total_cut, req.src, req.dst);
        const auto ba_arcs = pool.path_arcs(ba.path_id);
        bool in_pool = false;
        for (const PathId pid : cand) {
          if (locked_from_stage1) {
            const auto cand_arcs = pool.path_arcs(pid);
            if (cand_arcs.size() == ba_arcs.size() &&
                std::equal(cand_arcs.begin(), cand_arcs.end(),
                           ba_arcs.begin())) {
              in_pool = true;
              break;
            }
          } else if (pid == ba.path_id) {
            in_pool = true;
            break;
          }
        }
        REQUIRE(in_pool);

        const WavelengthMask bit = wavelength_bit(ba.wavelength);
        for (const ArcId a : pool.path_arcs(ba.path_id)) {
          REQUIRE((scen_occ[a] & bit) == 0u);
          scen_occ[a] |= bit;
          back_only[a] |= bit;
        }
      }

      for (ArcId a = 0; a < m; ++a) {
        REQUIRE(back_only[a] == state.seq_stage2_backup_mask(sc.id, a));
      }
    }
  }

  REQUIRE(audited_s2_count == sc_set.seq_scenarios().size());
  REQUIRE(SpectralTelemetry::verify_refcounts_exact(state));
  REQUIRE(SpectralTelemetry::evaluate(state) ==
          SpectralTelemetry::evaluate_full_scan(state));
}

}  // namespace ton::test

#if !defined(TON_ASAN_ACTIVE)
void* operator new(std::size_t size) {
  if (ton::test::g_track_allocations) {
    ++ton::test::g_allocation_count;
  }
  void* ptr = std::malloc(size);
  if (!ptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

void* operator new[](std::size_t size) {
  if (ton::test::g_track_allocations) {
    ++ton::test::g_allocation_count;
  }
  void* ptr = std::malloc(size);
  if (!ptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

void operator delete(void* ptr) noexcept { std::free(ptr); }

void operator delete[](void* ptr) noexcept { std::free(ptr); }

void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }

void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
#endif
