---
name: ton-invariant-auditor
description: >-
  Audits simulation output step logs against the 5 mathematical invariants (INV-1 capacity clash, INV-2 continuous wavelength, INV-3 disruption truth, INV-4 path admissibility, INV-5 failure fairness). Use during Phase 4 or after running experiment batches.
---

# ToN Invariant Audit Skill

Use this skill to audit simulation trajectories and verify 100% compliance with the 5 formal invariants defined in `PROBLEM.md` Section 4.

## The 5 Invariants to Verify

1. **INV-1: Zero-Capacity Clash & Spatial Exclusion**
   - For every physical link $\ell$ and wavelength $\lambda \in [0, 31]$, at most ONE primary lightpath is allocated.
   - For every scenario $f$, at most ONE active backup lightpath traverses $(\ell, \lambda)$.
   - Primary and backup lightpaths never occupy the same link $(\ell, \lambda)$.

2. **INV-2: Wavelength Continuity**
   - A single connection request $(s, d)$ must use the identical wavelength $\lambda$ on every hop of its primary route, and identical wavelength $\lambda_f$ on its backup route under scenario $f$.

3. **INV-3: Disruption Ground Truth**
   - A backup lightpath for request $r$ under scenario $f$ is active IF AND ONLY IF the primary path of $r$ intersects the cut links of scenario $f$:
     $$\mathcal{P}_{\text{prim}}(r) \cap L_{\text{cut}}(f) \neq \emptyset$$

4. **INV-4: Route Admissibility & Loop-Freedom**
   - Every primary and backup route must belong to the precomputed $(\kappa, K_{\max})$ candidate path pool, be simple (loop-free), and survive the scenario failure:
     $$\mathcal{P}_{\text{back}}(r, f) \cap L_{\text{cut}}(f) = \emptyset$$

5. **INV-5: Sequential Stage-1/Stage-2 Fairness**
   - Under sequential failure scenarios $(x_1, x_2)$, Stage-1 backup routes are evaluated strictly prior to Stage-2 disruptions, matching `algorithms/FAILURE_ORCHESTRATION.md`.

## Execution

Run the C++ invariant auditor on generated step logs:
```bash
./build/tests/invariant_audit_runner --results-dir results/raw_steps/
```
Or run the Python verification script:
```bash
.venv/bin/python scripts/analyze_results.py --verify-invariants --results-dir results/summaries/
```
Zero tolerance: any single invariant violation invalidates the run and requires debugging before proceeding.

