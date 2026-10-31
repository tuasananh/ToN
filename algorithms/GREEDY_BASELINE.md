# Comparative Baseline 1: Static Arrival-Order Greedy First-Fit (`GreedySimRDF` & `GreedySeqRDF`)

> **Document Purpose:** This document specifies the **`Greedy First-Fit`** baseline family—instantiated as **`GreedySimRDF`** under Simultaneous Orchestration and **`GreedySeqRDF`** under Sequential Orchestration (`algorithms/FAILURE_ORCHESTRATION.md`). It explains **why** this baseline is included in the paper, defines its exact mathematical allocation rule, and characterizes where it performs well versus where it breaks down relative to `Proposed DSATUR`.

---

## Table of Contents
1. [Purpose & Why This Baseline Is Used](#1-purpose--why-this-baseline-is-used)
2. [Theoretical Behavior: Sparse Graphs vs. Dense Hub Cliques](#2-theoretical-behavior-sparse-graphs-vs-dense-hub-cliques)
3. [Formal Mathematical Specification](#3-formal-mathematical-specification)
4. [Invariants & Time Complexity](#4-invariants--time-complexity)

---

## 1. Purpose & Why This Baseline Is Used

`GreedySimRDF` and `GreedySeqRDF` serve **two critical scientific roles** in `paper/ToN_2026.tex`:

1. **Verifying Cross-Algorithm Generality of Simultaneous vs. Sequential Trends:**
   Because the paper's central focus is comparing **simultaneous** versus **sequential** dual-link failure restoration, evaluating both `GreedySimRDF` and `GreedySeqRDF` proves that the structural capacity, blocking, and protection-overhead differences between simultaneous and sequential failures are **fundamental physical properties of the failure timing regimes**—not artifacts specific to our DSATUR coloring heuristic.
2. **Controlled Ablation of the DSATUR Coloring Engine:**
   `GreedySimRDF` and `GreedySeqRDF` retain **100% identical** failure orchestration, $(\kappa, K_{\max})$-bounded candidate route sets, full-scenario disrupted batches $U_F$, Stub-Release recycling, Stage-1 coupling, and Stage-2 Fixed Allocation protection as `SimRDF` and `SeqRDF`—replacing **only** the inner conflict-resolution solver. Any throughput or spectral-efficiency gap between `Proposed DSATUR` and `Greedy First-Fit` is therefore attributable purely to dynamic MRV/LCV/compaction coloring versus static First-Fit.

---

## 2. Theoretical Behavior: Sparse Graphs vs. Dense Hub Cliques

Understanding `Greedy First-Fit`'s structural strengths and failure modes explains its empirical behavior across topologies and traffic profiles:
- **Built-In Incumbent Stability:** Because requests in $U$ are processed strictly in ascending arrival order ($r_1 < r_2 < \dots$), older established requests always reclaim upper-spectrum channels ($\lambda_{W-1} \downarrow$) first, while the newly arriving request $k$ is evaluated last. On **sparse, planar-like topologies under uniform traffic**, conflict neighborhoods form low-treewidth forests where static First-Fit naturally achieves near-optimal packing with zero churn.
- **Blindness to Bottlenecks in Hub-Skewed Cliques:** Because `Greedy First-Fit` performs neither Minimum Remaining Values (MRV) ordering nor Least-Constraining Value (LCV) lookahead, an uncongested early request $r_1$ with $15$ free wavelengths blindly seizes $\lambda_{W-1}$ even when $\lambda_{W-1}$ is the **sole surviving valid wavelength** for a bottlenecked later request $r_j \in U$. Under realistic hub-concentrated traffic (e.g., core metropolitan corridors in Germany50 and Nobel-EU), dense conflict cliques cause static First-Fit to trigger preventable blocking and wider spectral spread.

---

## 3. Formal Mathematical Specification

Given degraded topology $G' = G \setminus L_{\text{fail}}$, disrupted request batch $U$, pre-locked active backups $A_{\text{fixed}}$, and locked surviving-primary slot set $S_{\text{locked}} \subseteq L' \times \Lambda$:

1. **Initialization:**
   Set $A \leftarrow A_{\text{fixed}}$ and initialize occupied slots:
   $$S(A) \leftarrow S_{\text{locked}} \cup \bigcup_{r' \in A} \big\{ (\ell, \lambda^b(r')) \;\big|\; \ell \in p^b(r') \big\}$$
2. **Static Arrival Ordering:**
   Sort all disrupted requests in $U$ in **strictly ascending order of arrival index**:
   $$(r_1, r_2, \dots, r_{|U|}) \quad \text{with} \quad r_1 < r_2 < \dots < r_{|U|}$$
3. **Sequential Descending First-Fit Assignment:**
   For $i = 1, 2, \dots, |U|$:
   - Retrieve candidate routes $\mathcal{C}(r_i) = \mathcal{P}(s_{r_i}, d_{r_i}; G', \kappa, K_{\max})$ ordered by non-decreasing route cost $c(p)$.
   - Scan routes $p \in \mathcal{C}(r_i)$ in order, and for each route scan wavelengths $\lambda$ in **descending order** ($\lambda_{W-1}, \lambda_{W-2}, \dots, \lambda_0$).
   - Select the **first** clash-free pair $(p^*, \lambda^*)$ satisfying $(\ell, \lambda^*) \notin S(A)$ for all $\ell \in p^*$.
   - If no clash-free pair exists for $r_i$, terminate immediately and return `Infeasible`.
   - Otherwise assign $\pi^b(r_i) \leftarrow (p^*, \lambda^*)$, update $A \leftarrow A \cup \{r_i\}$, and insert $\{(\ell, \lambda^*) \mid \ell \in p^*\}$ into $S(A)$.
4. **Completion:**
   Once all $r_i \in U$ are assigned, return $\{\pi^b(r)\}_{r \in A}$.

---

## 4. Invariants & Time Complexity

- **System Invariants:** Satisfies all five System Invariants in Section 9 of `PROBLEM.md` identically to `Proposed DSATUR`.
- **Per-State Time Complexity:** Each of the $|U|$ requests inspects at most $|\mathcal{C}| \le K_{\max}$ routes across $W$ wavelengths of hop length $\le \bar{H}$ with zero neighbor lookahead:
  $$T_{\text{state}}^{\text{Greedy}} = O\big( |U| \cdot |\mathcal{C}| \cdot W \cdot \bar{H} \big)$$
- **Per-Request Worst-Case Time Complexity:**
  $$T_{\text{req}}^{\text{Greedy}} = O\Big( |\mathcal{P}^w| \cdot W \cdot |\mathcal{F}_{\text{aff}}| \cdot |U| \cdot |\mathcal{C}| \cdot W \cdot \bar{H} \Big)$$

