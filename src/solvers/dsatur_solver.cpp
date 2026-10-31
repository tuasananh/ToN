#include "ton/solvers/dsatur_solver.hpp"

#include <algorithm>
#include <span>

namespace ton {

SolverStatus DsaturSolver::solve(const DegradedSubproblem& subproblem,
                                 SolverScratchpad& scratchpad,
                                 StagedAssignments& staged) const noexcept {
  TON_ASSERT(subproblem.path_pool != nullptr,
             "DsaturSolver requires non-null PathPool");

  const PathPool& pool = *subproblem.path_pool;
  const auto m = static_cast<ArcId>(subproblem.locked_masks.size());
  TON_BOUNDS_CHECK(m <= scratchpad.num_arcs(),
                   "DsaturSolver scratchpad num_arcs too small");

  std::copy(subproblem.locked_masks.begin(), subproblem.locked_masks.end(),
            scratchpad.occupied_mask.begin());

  // Initialize A <- A_fixed, S(A), and Lambda_active(A).
  WavelengthMask active_wavelengths = EMPTY_MASK;
  for (const BackupAssignment& fb : subproblem.fixed_backups) {
    TON_ASSERT(fb.valid(), "Invalid fixed BackupAssignment in DsaturSolver");
    const WavelengthMask bit = wavelength_bit(fb.wavelength);
    for (const ArcId a : pool.path_arcs(fb.path_id)) {
      TON_BOUNDS_CHECK(a < m, "Fixed backup ArcId out of bounds");
      if ((scratchpad.occupied_mask[a] & bit) != 0u) {
        return SolverStatus::Infeasible;
      }
      scratchpad.occupied_mask[a] |= bit;
    }
    active_wavelengths |= bit;
  }

  const auto num_u = static_cast<uint16_t>(subproblem.disrupted.size());
  if (num_u == 0u) {
    staged.stage_replace(subproblem.domain, subproblem.state_id,
                         subproblem.fixed_backups, {});
    return SolverStatus::Feasible;
  }
  TON_BOUNDS_CHECK(num_u <= scratchpad.max_disrupted(),
                   "DsaturSolver disrupted batch exceeds scratchpad capacity");

  std::fill_n(scratchpad.psi.begin(), static_cast<size_t>(m) * NUM_WAVELENGTHS,
              0.0);

  // Initialize free_mask[u, c], omega_lambda(u), and Omega(u).
  for (uint16_t u = 0; u < num_u; ++u) {
    const Request& req = subproblem.disrupted[u].req;
    const PathSlice slice = pool.find(subproblem.cut_arcs, req.src, req.dst);
    if (slice.empty()) {
      return SolverStatus::Infeasible;
    }
    TON_BOUNDS_CHECK(slice.count <= scratchpad.max_k() && slice.count <= 64u,
                     "Candidate route count exceeds scratchpad max_k");

    scratchpad.cand_slice[u] = slice;
    scratchpad.uncolored[u] = 1u;

    WavelengthMask u_union = EMPTY_MASK;
    uint16_t u_pairs = 0;
    for (uint16_t c = 0; c < slice.count; ++c) {
      WavelengthMask used = EMPTY_MASK;
      for (const ArcId a : pool.path_arcs(slice[c])) {
        TON_BOUNDS_CHECK(a < m, "Candidate backup ArcId out of bounds");
        used |= scratchpad.occupied_mask[a];
      }
      const WavelengthMask fm = ~used;
      scratchpad.route_free_mask(u, c) = fm;
      u_union |= fm;
      u_pairs = static_cast<uint16_t>(u_pairs + count_wavelengths(fm));
    }

    if (u_union == EMPTY_MASK) {
      return SolverStatus::Infeasible;
    }
    scratchpad.union_free_mask[u] = u_union;
    scratchpad.omega_lambda[u] = count_wavelengths(u_union);
    scratchpad.omega_pairs[u] = u_pairs;
  }

  // Theorem 2 initialization: accumulate Psi_U(l, lambda) across all u in U.
  for (uint16_t u = 0; u < num_u; ++u) {
    const double inv_omega =
        1.0 / static_cast<double>(scratchpad.omega_pairs[u]);
    const PathSlice slice = scratchpad.cand_slice[u];
    for (uint16_t c = 0; c < slice.count; ++c) {
      const WavelengthMask fm = scratchpad.route_free_mask(u, c);
      if (fm == EMPTY_MASK) {
        continue;
      }
      for (const ArcId a : pool.path_arcs(slice[c])) {
        double* psi_row =
            scratchpad.psi.data() + static_cast<size_t>(a) * NUM_WAVELENGTHS;
        WavelengthMask m_w = fm;
        while (m_w != EMPTY_MASK) {
          const uint8_t w = lowest_wavelength(m_w);
          m_w &= (m_w - 1u);
          psi_row[w] += inv_omega;
        }
      }
    }
  }

  // Iterative Two-Tier Exact MRV coloring loop.
  for (uint16_t step = 0; step < num_u; ++step) {
    uint16_t r_star = 0xFFFFu;
    uint8_t best_wl = 0xFFu;
    uint16_t best_wp = 0xFFFFu;
    RequestId best_req_id = INVALID_REQUEST;

    for (uint16_t u = 0; u < num_u; ++u) {
      if (scratchpad.uncolored[u] == 0u) {
        continue;
      }
      const uint8_t wl = scratchpad.omega_lambda[u];
      if (wl == 0u) {
        return SolverStatus::Infeasible;
      }
      const uint16_t wp = scratchpad.omega_pairs[u];
      const RequestId rid = subproblem.disrupted[u].req.id;
      if (r_star == 0xFFFFu || wl < best_wl ||
          (wl == best_wl &&
           (wp < best_wp || (wp == best_wp && rid < best_req_id)))) {
        r_star = u;
        best_wl = wl;
        best_wp = wp;
        best_req_id = rid;
      }
    }

    TON_ASSERT(r_star < num_u, "MRV failed to select an uncolored request");

    // Subtract r_star's own potential from Psi to obtain Psi_{-r*}(l, lambda).
    const double inv_rstar_omega = 1.0 / static_cast<double>(best_wp);
    const PathSlice r_slice = scratchpad.cand_slice[r_star];
    for (uint16_t c = 0; c < r_slice.count; ++c) {
      const WavelengthMask fm = scratchpad.route_free_mask(r_star, c);
      if (fm == EMPTY_MASK) {
        continue;
      }
      for (const ArcId a : pool.path_arcs(r_slice[c])) {
        double* psi_row =
            scratchpad.psi.data() + static_cast<size_t>(a) * NUM_WAVELENGTHS;
        WavelengthMask m_w = fm;
        while (m_w != EMPTY_MASK) {
          const uint8_t w = lowest_wavelength(m_w);
          m_w &= (m_w - 1u);
          psi_row[w] = std::max(0.0, psi_row[w] - inv_rstar_omega);
        }
      }
    }

    // Theorem 1: O(1) Forward-Checking bypass whenever omega_lambda(r*) >= 2.
    uint16_t num_crit = 0;
    WavelengthMask crit_bit = EMPTY_MASK;
    if (best_wl >= 2u) {
      ++scratchpad.theorem1_bypass_steps;
    } else {
      ++scratchpad.theorem1_fc_checked_steps;
      crit_bit = scratchpad.union_free_mask[r_star];
      TON_ASSERT(count_wavelengths(crit_bit) == 1u,
                 "omega_lambda == 1 must correspond to single bit in union");
      for (uint16_t u = 0; u < num_u; ++u) {
        if (u != r_star && scratchpad.uncolored[u] != 0u &&
            scratchpad.union_free_mask[u] == crit_bit) {
          scratchpad.crit_neighbors[num_crit++] = u;
        }
      }
    }

    // Score wipeout-safe candidates (p, lambda) in V_safe(r*) via unified
    // 4-tuple CandidateScore.
    bool found_safe = false;
    PathId best_path = INVALID_PATH;
    uint8_t best_w = INVALID_WAVELENGTH;
    CandidateScore best_score{};

    const Lightpath old_lp = subproblem.disrupted[r_star].old_backup;
    const bool has_old = old_lp.valid();

    for (uint16_t c = 0; c < r_slice.count; ++c) {
      WavelengthMask fm = scratchpad.route_free_mask(r_star, c);
      if (fm == EMPTY_MASK) {
        continue;
      }

      const PathId pid = r_slice[c];
      const auto p_arcs = pool.path_arcs(pid);

      if (num_crit > 0u) {
        const uint32_t stamp = scratchpad.next_arc_stamp();
        for (const ArcId a : p_arcs) {
          scratchpad.arc_stamp[a] = stamp;
        }

        bool wipes_out_neighbor = false;
        for (uint16_t k_idx = 0; k_idx < num_crit; ++k_idx) {
          const uint16_t u_crit = scratchpad.crit_neighbors[k_idx];
          const PathSlice u_slice = scratchpad.cand_slice[u_crit];
          bool u_has_disjoint_route = false;

          for (uint16_t uc = 0; uc < u_slice.count; ++uc) {
            if ((scratchpad.route_free_mask(u_crit, uc) & crit_bit) == 0u) {
              continue;
            }
            bool intersects = false;
            for (const ArcId ua : pool.path_arcs(u_slice[uc])) {
              if (scratchpad.arc_stamp[ua] == stamp) {
                intersects = true;
                break;
              }
            }
            if (!intersects) {
              u_has_disjoint_route = true;
              break;
            }
          }

          if (!u_has_disjoint_route) {
            wipes_out_neighbor = true;
            break;
          }
        }

        if (wipes_out_neighbor) {
          continue;
        }
      }

      const double p_cost = pool.path_cost(pid);
      while (fm != EMPTY_MASK) {
        const uint8_t w = highest_wavelength(fm);
        const WavelengthMask w_bit = wavelength_bit(w);
        fm &= ~w_bit;

        double damage = 0.0;
        for (const ArcId a : p_arcs) {
          damage +=
              scratchpad.psi[static_cast<size_t>(a) * NUM_WAVELENGTHS + w];
        }

        const double churn =
            (has_old && (old_lp.wavelength != w ||
                         !same_physical_path(pool, old_lp.path_id, pid)))
                ? kBetaInertia
                : 0.0;

        const CandidateScore score{
            .total_damage = damage + churn,
            .path_cost = p_cost,
            .is_new_channel = static_cast<uint8_t>(
                (active_wavelengths & w_bit) != 0u ? 0u : 1u),
            .wavelength_rank = static_cast<uint8_t>(NUM_WAVELENGTHS - 1u - w),
        };

        if (!found_safe || score.is_better_than(best_score)) {
          found_safe = true;
          best_score = score;
          best_path = pid;
          best_w = w;
        }
      }
    }

    if (!found_safe) {
      return SolverStatus::Infeasible;
    }

    // Commit (best_path, best_w) to r_star and incrementally update colliding
    // neighbors d_hit <= |U|.
    scratchpad.solved_backups[step] = BackupAssignment{
        .path_id = best_path,
        .req_id = best_req_id,
        .wavelength = best_w,
    };
    scratchpad.uncolored[r_star] = 0u;

    const WavelengthMask star_bit = wavelength_bit(best_w);
    active_wavelengths |= star_bit;

    if (step + 1u == num_u) {
      break;
    }

    const uint32_t assign_stamp = scratchpad.next_arc_stamp();
    for (const ArcId a : pool.path_arcs(best_path)) {
      scratchpad.occupied_mask[a] |= star_bit;
      scratchpad.arc_stamp[a] = assign_stamp;
    }

    for (uint16_t u = 0; u < num_u; ++u) {
      if (scratchpad.uncolored[u] == 0u) {
        continue;
      }
      if ((scratchpad.union_free_mask[u] & star_bit) == 0u) {
        continue;
      }

      const PathSlice u_slice = scratchpad.cand_slice[u];
      uint64_t lost_route_mask = 0u;
      uint16_t hit_routes = 0;

      for (uint16_t uc = 0; uc < u_slice.count; ++uc) {
        if ((scratchpad.route_free_mask(u, uc) & star_bit) == 0u) {
          continue;
        }
        for (const ArcId ua : pool.path_arcs(u_slice[uc])) {
          if (scratchpad.arc_stamp[ua] == assign_stamp) {
            lost_route_mask |= (uint64_t{1} << uc);
            ++hit_routes;
            break;
          }
        }
      }

      if (hit_routes == 0u) {
        continue;
      }

      const uint16_t old_omega = scratchpad.omega_pairs[u];
      TON_ASSERT(old_omega >= hit_routes, "omega_pairs underflow");
      const uint16_t new_omega = static_cast<uint16_t>(old_omega - hit_routes);
      if (new_omega == 0u) {
        return SolverStatus::Infeasible;
      }

      const double old_inv = 1.0 / static_cast<double>(old_omega);
      const double delta_keep =
          (1.0 / static_cast<double>(new_omega)) - old_inv;

      WavelengthMask new_union = EMPTY_MASK;
      for (uint16_t uc = 0; uc < u_slice.count; ++uc) {
        WavelengthMask fm = scratchpad.route_free_mask(u, uc);
        const bool lost = (lost_route_mask & (uint64_t{1} << uc)) != 0u;
        if (lost) {
          fm &= ~star_bit;
          scratchpad.route_free_mask(u, uc) = fm;
        }
        new_union |= fm;

        if (fm == EMPTY_MASK && !lost) {
          continue;
        }

        for (const ArcId ua : pool.path_arcs(u_slice[uc])) {
          double* psi_row =
              scratchpad.psi.data() + static_cast<size_t>(ua) * NUM_WAVELENGTHS;
          if (lost) {
            psi_row[best_w] = std::max(0.0, psi_row[best_w] - old_inv);
          }
          WavelengthMask m_w = fm;
          while (m_w != EMPTY_MASK) {
            const uint8_t w = lowest_wavelength(m_w);
            m_w &= (m_w - 1u);
            psi_row[w] += delta_keep;
          }
        }
      }

      scratchpad.union_free_mask[u] = new_union;
      scratchpad.omega_lambda[u] = count_wavelengths(new_union);
      scratchpad.omega_pairs[u] = new_omega;
    }
  }

  staged.stage_replace(subproblem.domain, subproblem.state_id,
                       subproblem.fixed_backups,
                       std::span<const BackupAssignment>(
                           scratchpad.solved_backups.data(), num_u));
  return SolverStatus::Feasible;
}

}  // namespace ton
