# Failure-Regime Provisioning Orchestration: Simultaneous vs. Sequential

> **Document Purpose:** This document specifies the outer failure-state orchestration shared identically across all restoration heuristics (`Proposed DSATUR`, `Greedy First-Fit`, and `Immutable No-Reroute`). Separating the physical failure-regime orchestration from the inner conflict-resolution solver guarantees that every comparison between algorithms operates under identical scenario spaces, candidate path sets, Stub-Release rules, and sequential Stage-1/Stage-2 causality.

---

## Table of Contents
1. [Role & Why Two Orchestration Regimes Are Modeled](#1-role--why-two-orchestration-regimes-are-modeled)
2. [Simultaneous Single-Stage Orchestration ($\mathcal{F}_{\text{sim}}$)](#2-simultaneous-single-stage-orchestration-mathcalf_textsim)
3. [Sequential Two-Stage Coupled Orchestration ($\mathcal{F}_{\text{seq}}$)](#3-sequential-two-stage-coupled-orchestration-mathcalf_textseq)
4. [Rationale for Full-Scenario Disrupted Batching ($U_F$)](#4-rationale-for-full-scenario-disrupted-batching-u_f)

---

## 1. Role & Why Two Orchestration Regimes Are Modeled

The central scientific objective of `paper/ToN_2026.tex` is to compare proactive optical restoration under **simultaneous** versus **sequential** dual-link failures:
- **Simultaneous Orchestration** models concurrent dual cuts $\{x_1, x_2\} \in \mathcal{F}_{\text{sim}}$ where both failure units are already severed before recovery initiates. Each unordered pair is an independent single-stage restoration subproblem.
- **Sequential Orchestration** models realistic staggered cuts $(x_1, x_2) \in \mathcal{F}_{\text{seq}}$ where $x_2$ fails during the Mean Time to Repair of $x_1$. Because the optical control plane observes **only $x_1$** during Stage 1, $x_2$ remains healthy and usable during Stage-1 recovery, coupling all scenarios in $\mathcal{G}_{x_1} = \{(x_1, x_2) \in \mathcal{F}_{\text{seq}}\}$ to a **single Stage-1 allocation** and locking any Stage-1 backup that avoids $x_2$ during Stage 2.

Both regimes plug into the unified online primary trial loop defined in `ALGORITHMS.md` Section 1: for a clash-free candidate primary $\pi^w_k = (p^w_k, \lambda^w_k)$ with $p^w_k \in \mathcal{P}(s_k, d_k; G, \kappa, K_{\max})$ and $\lambda^w_k \in \{\lambda_0, \dots, \lambda_{W-1}\}$, the orchestrator identifies all affected failure states and invokes the chosen inner solver (`DSATUR`, `Greedy First-Fit`, or `No-Reroute`) on each state.

---

## 2. Simultaneous Single-Stage Orchestration ($\mathcal{F}_{\text{sim}}$)

### 2.1 Affected Scenario Identification
Given trial primary lightpath $\pi^w_k = (p^w_k, \lambda^w_k)$:
1. **Primary-Severance Scenarios ($\mathcal{F}_{\text{rest}}$):** Unordered scenarios that cut $p^w_k$, requiring a backup lightpath for incoming request $k$:
   $$\mathcal{F}_{\text{rest}} = \big\{ F \in \mathcal{F}_{\text{sim}} \;\big|\; p^w_k \cap L(F) \neq \emptyset \big\}$$
2. **Backup-Collision Scenarios ($\mathcal{F}_{\text{inter}}$):** Unordered scenarios in which $p^w_k$ survives ($p^w_k \cap L(F) = \emptyset$), claiming working slots $\{(\ell, \lambda^w_k) \mid \ell \in p^w_k\}$ that collide with at least one existing backup reservation in $F$:
   $$\mathcal{F}_{\text{inter}} = \Big\{ F \in \mathcal{F}_{\text{sim}} \setminus \mathcal{F}_{\text{rest}} \;\Big|\; \exists r \in K_{\text{adm}} \text{ with } p^w_r \cap L(F) \neq \emptyset, \; \lambda^b_r(F) = \lambda^w_k, \text{ and } p^w_k \cap p^b_r(F) \neq \emptyset \Big\}$$
3. **Total Affected Scenarios:** $\mathcal{F}_{\text{aff}} = \mathcal{F}_{\text{rest}} \cup \mathcal{F}_{\text{inter}}$. Scenarios outside $\mathcal{F}_{\text{aff}}$ remain unaffected and clash-free.

### 2.2 Step-by-Step Simultaneous Resolution
For each affected scenario $F \in \mathcal{F}_{\text{aff}}$ (ordered by conflict-directed fail-fast priority; see `EXPERIMENTS.md` Section 3.2):
1. **Degraded Graph & Locked Slots:** Set $G' = G \setminus L(F)$ and $A_{\text{fixed}} = \emptyset$. Construct $S_{\text{locked}}$ from all surviving committed primaries $\{(\ell, \lambda^w_r) \mid r \in K_{\text{adm}}, \; p^w_r \cap L(F) = \emptyset, \; \ell \in p^w_r\}$ (omitting severed primaries via **Stub-Release**), plus $\{(\ell, \lambda^w_k) \mid \ell \in p^w_k\}$ when $F \in \mathcal{F}_{\text{inter}}$.
2. **Disrupted Request Batch ($U_F$):**
   - For dynamic-reconfiguration solvers (`SimRDF`, `GreedySimRDF`):
     $$U_F = \big\{ r \in K_{\text{adm}} \;\big|\; p^w_r \cap L(F) \neq \emptyset \big\} \cup \begin{cases} \{k\} & \text{if } F \in \mathcal{F}_{\text{rest}} \\ \emptyset & \text{if } F \in \mathcal{F}_{\text{inter}} \end{cases}$$
   - For the immutable baseline (`NoRerouteSimRDF`): If $\mathcal{F}_{\text{inter}} \neq \emptyset$, reject $(p^w_k, \lambda^w_k)$ immediately; otherwise $U_F = \{k\}$ for $F \in \mathcal{F}_{\text{rest}}$, with all existing backups in $F$ added to $S_{\text{locked}}$.
3. **Inner Solver Invocation:** Invoke the inner conflict-resolution solver on $(G', U_F, A_{\text{fixed}}, S_{\text{locked}})$. If any $F \in \mathcal{F}_{\text{aff}}$ returns `Infeasible`, abort the $(p^w_k, \lambda^w_k)$ trial immediately.
4. **Commit:** If all $F \in \mathcal{F}_{\text{aff}}$ succeed, atomically commit $\pi^w_k$ and all updated backup allocations $\{\pi^b_r(F)\}$.

---

## 3. Sequential Two-Stage Coupled Orchestration ($\mathcal{F}_{\text{seq}}$)

### 3.1 Two-Stage Conflict Identification
Ordered scenarios $(x_1, x_2) \in \mathcal{F}_{\text{seq}}$ are partitioned by first cut $x_1 \in \mathcal{E}_1$ into coupled groups $\mathcal{G}_{x_1} = \{(x_1, x_2) \in \mathcal{F}_{\text{seq}}\}$. Given trial primary $\pi^w_k = (p^w_k, \lambda^w_k)$:
1. **Affected Stage-1 Units ($\mathcal{E}^{\text{aff}}_{s1} \subseteq \mathcal{E}_1$):** First cuts $x_1$ that either sever $p^w_k$ or suffer a collision between surviving $p^w_k$ and an existing Stage-1 backup on $\lambda^w_k$:
   $$\mathcal{E}^{\text{aff}}_{s1} = \Big\{ x_1 \in \mathcal{E}_1 \;\Big|\; p^w_k \cap L_{\text{cut}}(x_1) \neq \emptyset \;\lor\; \big(\exists r \in K_{\text{adm}} : p^w_r \cap L_{\text{cut}}(x_1) \neq \emptyset, \; \lambda^{s1}_r(x_1) = \lambda^w_k, \; p^w_k \cap p^{s1}_r(x_1) \neq \emptyset\big) \Big\}$$
2. **Directly Affected Stage-2 Scenarios ($\mathcal{F}^{\text{aff}}_{s2} \subseteq \mathcal{F}_{\text{seq}}$):** Ordered scenarios $F = (x_1, x_2)$ with $x_1 \notin \mathcal{E}^{\text{aff}}_{s1}$ where Stage 1 is unchanged, but either $x_2$ severs surviving $p^w_k$ or surviving $p^w_k$ collides with an existing Stage-2 backup on $\lambda^w_k$:
   $$\mathcal{F}^{\text{aff}}_{s2} = \Big\{ F = (x_1, x_2) \in \mathcal{F}_{\text{seq}} \;\Big|\; x_1 \notin \mathcal{E}^{\text{aff}}_{s1} \text{ and } \Big( p^w_k \cap L_{\text{cut}}(x_2) \neq \emptyset \;\lor\; \big(\exists r \in K_{\text{adm}} : p^w_r \cap L(F) \neq \emptyset, \; \lambda^{s2}_r(F) = \lambda^w_k, \; p^w_k \cap p^{s2}_r(F) \neq \emptyset\big) \Big) \Big\}$$

### 3.2 Strict Single-Pass Stage-1 Causality & All-Stage-1-First Gating
To mirror physical causality—where Stage-1 restoration reacts to $x_1$ before $x_2$ occurs—and keep computational complexity tractable across tens of thousands of scenarios, **all three sequential algorithms (`SeqRDF`, `GreedySeqRDF`, and `NoRerouteSeqRDF`) enforce strict single-pass Stage-1 resolution**:
- **Phase A (Resolve All Affected Stage-1 Units First):**
  For every $x_1 \in \mathcal{E}^{\text{aff}}_{s1}$, resolve Stage-1 restoration on $G \setminus L_{\text{cut}}(x_1)$ in **a single invocation** using only Stage-1 state ($x_2$ healthy, $A_{\text{fixed}} = \emptyset$).
  - For dynamic solvers (`SeqRDF`, `GreedySeqRDF`), the batch is $U^{(1)}_{x_1} = \{r \in K_{\text{adm}} \mid p^w_r \cap L_{\text{cut}}(x_1) \neq \emptyset\} \cup (\{k\} \text{ if } p^w_k \cap L_{\text{cut}}(x_1) \neq \emptyset)$.
  - For `NoRerouteSeqRDF`, if any collision occurs with an immutable Stage-1 or Stage-2 backup on a surviving primary, $(p^w_k, \lambda^w_k)$ is rejected immediately; otherwise $U^{(1)}_{x_1} = \{k\}$ for $p^w_k \cap L_{\text{cut}}(x_1) \neq \emptyset$.
  - If **any** $x_1 \in \mathcal{E}^{\text{aff}}_{s1}$ returns `Infeasible` in Stage 1, abort $(p^w_k, \lambda^w_k)$ immediately before entering Stage 2.
  - For $x_1 \notin \mathcal{E}^{\text{aff}}_{s1}$, retain committed Stage-1 allocations $\tilde{\pi}^{s1}_r(x_1) = \pi^{s1}_r(x_1)$.

- **Phase B (Resolve Stage-2 Scenarios Subject to Locked Surviving Stage-1 Backups):**
  Evaluate every scenario $F = (x_1, x_2) \in \mathcal{F}_{\text{seq}}$ satisfying $x_1 \in \mathcal{E}^{\text{aff}}_{s1}$ or $F \in \mathcal{F}^{\text{aff}}_{s2}$ on $G' = G \setminus L(F)$:
  1. **Partition Stage-1 Restored Requests ($U^{(1)}_{x_1}$):**
     - *Locked Surviving Stage-1 Backups:*
       $$A_{\text{fixed}}(F) = \big\{ r \in U^{(1)}_{x_1} \;\big|\; \tilde{p}^{s1}_r(x_1) \cap L_{\text{cut}}(x_2) = \emptyset \big\}, \qquad \tilde{\pi}^{s2}_r(F) = \tilde{\pi}^{s1}_r(x_1)$$
       *(Note: Under `NoRerouteSeqRDF`, if $k \in A_{\text{fixed}}(F)$ and $\tilde{\pi}^{s1}_k(x_1)$ shares a link-wavelength slot with an existing immutable Stage-2 backup in $F$, Stage 2 immediately returns `Infeasible` and aborts $(p^w_k, \lambda^w_k)$.)*
     - *Double-Disrupted Requests (Stage-1 Backup Severed by $x_2$):*
       $$U^{(2,\text{broken})}_F = \big\{ r \in U^{(1)}_{x_1} \;\big|\; \tilde{p}^{s1}_r(x_1) \cap L_{\text{cut}}(x_2) \neq \emptyset \big\}$$
       *(Their severed Stage-1 backup slots are extinguished and released.)*
  2. **Primaries Surviving $x_1$ and Severed by $x_2$:**
     $$U^{(2,\text{prim})}_F = \Big\{ r \in K_{\text{adm}} \cup \{k\} \;\Big|\; p^w_r \cap L_{\text{cut}}(x_1) = \emptyset \text{ and } p^w_r \cap L_{\text{cut}}(x_2) \neq \emptyset \Big\}$$
  3. **Invoke Inner Solver:**
     - Dynamic solvers (`SeqRDF`, `GreedySeqRDF`) solve $U_F = U^{(2,\text{broken})}_F \cup U^{(2,\text{prim})}_F$ around $A_{\text{fixed}}(F)$ and surviving primaries $S_{\text{locked}}$.
     - `NoRerouteSeqRDF` solves $U_F = (U^{(2,\text{broken})}_F \cup U^{(2,\text{prim})}_F) \cap \{k\}$ with all existing Stage-2 backups locked in $S_{\text{locked}}$.
     - If any $F$ returns `Infeasible`, abort $(p^w_k, \lambda^w_k)$ immediately.
  4. **Commit:** If all evaluated Stage-1 and Stage-2 states succeed, commit $\pi^w_k$, $\{\tilde{\pi}^{s1}_r(x_1)\}$, and $\{\tilde{\pi}^{s2}_r(F)\}$.

---

## 4. Rationale for Full-Scenario Disrupted Batching ($U_F$)

When dynamic backup reconfiguration is enabled (`DSATUR` and `Greedy First-Fit`), placing **all** non-fixed disrupted requests of scenario $F$ into $U_F$—rather than recoloring only the single backup directly hit by a surviving primary—is theoretically essential for two reasons:
1. **Multi-Step Cascade Repacking:** If existing backup $r_1$ on $\lambda_{31}$ is displaced by a surviving new primary and $r_1$'s only alternative slot is $\lambda_{30}$ currently held by $r_2$, full-batch recoloring allows $r_2$ to shift cleanly to $\lambda_{29}$ so $r_1$ can take $\lambda_{30}$, admitting requests that single-hop recoloring would falsely block.
2. **Zero Unnecessary Churn Under Incumbency Stability:** In `DSATUR`, the parameter-free incumbency penalty ($\beta_{\text{inertia}} = 1/W$) ensures requests in $U_F$ that do not impede constrained neighbors remain locked on their existing slots, combining full multi-step repacking flexibility with minimal spectrum disturbance.

