# AGENTS.md — ToN 2026 Core Engineering & Long-Horizon Operational Contract

> **Mission:** Deliver the complete C++23 high-performance survivability engine and 1,704-run experimental campaign specified in `PLAN.md`, strictly honoring the theoretical foundations of `PROBLEM.md` and `algorithms/*.md`.

---

## 1. Document Authority & Immutability Hierarchy

1. **Theoretical Foundations (STRICTLY READ-ONLY / IMMUTABLE):**
   - `PROBLEM.md`: Mathematical definitions, optimization objectives, metric equations, and the 5 invariants (`INV-1` to `INV-5`).
   - `ALGORITHMS.md` & `algorithms/*.md`: Theorem 1 & 2 formulations, heuristic definitions (DSATUR, Greedy, No-Reroute), and failure orchestration logic.
   - `EXPERIMENTS.md`: Fairness protocols, PRNG seed specifications, Wilcoxon statistical methodology, and output schemas.
   - *Never edit, rewrite, or simplify mathematical definitions or formulas in these files.*

2. **Engineering Execution Roadmap:**
   - `PLAN.md`: Canonical implementation roadmap (Phases 1–6), cache-residency specifications, directory structures, and verification gates.

---

## 2. C++23 High-Performance Architecture Invariants

1. **Toolchain & Standards:**
   - Target C++23: `GCC 16.2.1` / `Clang 23.1.4` with flags `-std=c++23 -O3 -march=native -flto -DNDEBUG`.
   - Build system: Modern `CMake 4.4.5` targets with `-Wall -Wextra -Wpedantic -Werror`.

2. **Zero-Allocation Online Loop:**
   - **No dynamic memory allocations** (`new`, `malloc`, `std::vector::push_back`, `std::make_shared`, `std::string` copies) inside the request provisioning loop ($t = 0 \dots 999$) or failure evaluation orchestrators.
   - Pre-allocate all solver scratchpads (`free_mask`, $\omega_{\lambda}$, $\Omega(u)$, contention matrix $\Psi[\ell][\lambda]$) per worker thread before starting the arrival stream.

3. **Bitmask Word Mapping & Cache Residency:**
   - Because $W = 32$, every link's wavelength occupancy maps 1-to-1 onto `using WavelengthMask = uint32_t` (`FULL_MASK = 0xFFFFFFFFu`). No dynamic bitsets or padding overhead.
   - Maintain L1-resident state (`primary_mask`, `primary_owner`, `backup_slot_refcount`) $< 12\text{ KB}$.
   - Ensure maximum scenario buffer size stays within the Intel i9-13900HX unified L3 cache ($36\text{ MiB}$).

4. **Deterministic Tie-Breaking & Reproducibility:**
   - All shortest-path calculations (Yen's $K$-shortest paths) must enforce strict deterministic lexicographic node-ID tie-breaking on equal-cost routes.
   - Floating-point Haversine calculations must use identical precision constants.

---

## 3. Mandatory Phase-Gate Verification Protocol

Before declaring any phase complete or moving to the next phase in `PLAN.md`, the agent must satisfy the following gate:

1. **Phase 1 Gate (Datasets & Streams):**
   - Official SNDlib topologies parsed and validated.
   - All $112$ base streams ($4 \times 28$) and $60$ permutation streams generated and frozen on disk.
   - Canonical `SHA-256` manifest generated and verified.

2. **Phase 2 Gate (Core Graph & Scenario Counts):**
   - Symmetric digraph and Haversine km distances validated.
   - Dual-cut scenario enumerator + Tarjan SCC filter must **hard-assert exact scenario counts**:
     - `Nobel-EU`: `804 / 1,608` (Span), `3,289 / 6,578` (Directional)
     - `Germany50`: `3,817 / 7,634` (Span), `15,378 / 30,756` (Directional)
   - Yen's path pool generated with $\kappa = 0.10, K_{\max} = 10$.

3. **Phase 3 Gate (Solvers & Orchestrators):**
   - Unit tests confirm exact behavioral match with `algorithms/DSATUR_HEURISTIC.md` (Theorem 1 bypass & Theorem 2 link-factored contention), `algorithms/GREEDY_BASELINE.md`, and `algorithms/NO_REROUTE_BASELINE.md`.
   - Simultaneous and sequential orchestrators pass transactional admission and rollback tests.
   - $O(1)$ `backup_slot_refcount` spectral logger matches full state scans.

4. **Phase 4 Gate (5-Invariant Verifier):**
   - Full automated auditor confirms 100% compliance with `INV-1` through `INV-5`.
   - Fuzz testing with random disruptions finds zero undetected invariant violations.

5. **Atomic, Bite-Sized Commit Protocol (Mandatory):**
   - **Never make monolithic multi-file phase dumps.** Break every phase down into bite-sized, atomic micro-deliverables.
   - Commit immediately whenever a discrete unit, test, or sub-component passes verification.
   - Every commit must leave the repository in a **working, passing state** (tests pass, zero warnings).
   - Use standard Conventional Commits:
     - `test(phase-X): <add unit tests or assertion checks for component Y>`
     - `feat(phase-X): <implement component Y matching spec>`
     - `fix(phase-X): <resolve specific bug or assertion failure in Y>`
     - `refactor(phase-X): <optimize memory or performance without changing behavior>`
     - `chore(phase-X): <freeze datasets, manifests, or update ledger>`

---

## 4. Python Environment & Script Execution

- Always use the dedicated virtual environment:
  `.venv/bin/python <script_path>` (or `source .venv/bin/activate`).
- Python is used strictly offline for SNDlib ingestion, PRNG stream freezing, Wilcoxon statistical tests, and LaTeX/figure generation.

