# Comparative Baseline 2: Immutable Backup Provisioning (`NoRerouteSimRDF` & `NoRerouteSeqRDF`)

> **Document Purpose:** This document specifies the **`Immutable No-Reroute`** baseline family—instantiated as **`NoRerouteSimRDF`** under Simultaneous Orchestration and **`NoRerouteSeqRDF`** under Sequential Orchestration (`algorithms/FAILURE_ORCHESTRATION.md`). It explains **why** an immutable-backup baseline is evaluated, formalizes its strict single-pass Stage-1 and Stage-2 rules, and derives its theoretical complexity.

---

## Table of Contents
1. [Purpose & Why This Baseline Is Used](#1-purpose--why-this-baseline-is-used)
2. [Theoretical Consequences of Freezing Backup Reservations](#2-theoretical-consequences-of-freezing-backup-reservations)
3. [Formal Mathematical Specification](#3-formal-mathematical-specification)
4. [Invariants & Time Complexity](#4-invariants--time-complexity)

---

## 1. Purpose & Why This Baseline Is Used

`NoRerouteSimRDF` and `NoRerouteSeqRDF` serve **two essential scientific purposes** in `paper/ToN_2026.tex`:

1. **Testing Simultaneous vs. Sequential Dynamics Without Control-Plane Reconfiguration:**
   In legacy or ultra-conservative optical control planes, operators freeze pre-planned restoration tables upon admission so existing backup cross-connect schedules never change when future requests arrive. Evaluating `NoRerouteSimRDF` against `NoRerouteSeqRDF` verifies whether simultaneous-vs.-sequential capacity and overhead laws persist even when dynamic backup rerouting is completely disabled.
2. **Quantifying the Exact Value of Dynamic Backup Reconfiguration:**
   In both `Proposed DSATUR` and `Greedy First-Fit`, previously planned dark backup reservations may be dynamically reconfigured upon admitting a new request. Comparing `Greedy First-Fit` / `Proposed DSATUR` against `No-Reroute` directly measures how much network throughput ($K_{\text{granted}}$) and lossless headroom ($I_{\text{denial}}$) are sacrificed when existing backup reservations are treated as immutable obstacles.

---

## 2. Theoretical Consequences of Freezing Backup Reservations

Disabling backup reconfiguration imposes two compounding constraints on incoming request $k$:
1. **Surviving-Primary Collision Blocking:** Even when primary capacity is abundant near $\lambda_0 \uparrow$, as working traffic grows toward the middle of the spectrum it encounters upper-spectrum slots $(\ell, \lambda^w_k)$ previously reserved by some admitted request $r$'s backup under scenarios $F$ that **do not cut $p^w_k$**. Because $p^w_k$ survives $F$ and $r$'s backup in $F$ cannot be moved to another free channel, $(p^w_k, \lambda^w_k)$ is immediately blocked.
2. **Sequential Stage-1 $\to$ Stage-2 Lock-In Collisions (`NoRerouteSeqRDF`):** Under strict single-pass Stage-1 causality, incoming request $k$ (when severed by $x_1$) selects its first clash-free Stage-1 backup $\tilde{\pi}^{s1}_k(x_1)$ on $G \setminus L_{\text{cut}}(x_1)$. For every second cut $x_2$ that $\tilde{p}^{s1}_k(x_1)$ avoids, $\tilde{\pi}^{s1}_k(x_1)$ becomes **locked in Stage 2** under $(x_1, x_2)$—where it can collide directly with an existing request $r_2$'s **immutable Stage-2 backup** $\pi^{s2}_{r_2}((x_1, x_2))$ (from a request $r_2$ whose primary survived $x_1$ and was only cut by $x_2$, leaving that slot free during Stage 1!). Because $r_2$'s Stage-2 backup cannot yield, Stage 2 fails and rejects $(p^w_k, \lambda^w_k)$.

---

## 3. Formal Mathematical Specification

All committed primary lightpaths **and** all committed backup lightpaths of admitted requests $r \in K_{\text{adm}}$ are strictly immutable ($U$ contains at most the single incoming request $\{k\}$).

### 3.1 `NoRerouteSimRDF` (Simultaneous Immutable Baseline)
For a clash-free trial primary $\pi^w_k = (p^w_k, \lambda^w_k)$:
1. **Surviving-Primary Collision Gate:** If $\mathcal{F}_{\text{inter}} \neq \emptyset$—meaning $\exists F \in \mathcal{F}_{\text{sim}}$ with $p^w_k \cap L(F) = \emptyset$ where $(p^w_k, \lambda^w_k)$ shares a link-wavelength slot with an immutable backup $\pi^b_r(F)$—reject $(p^w_k, \lambda^w_k)$ immediately.
2. **Single-Request First-Fit Backup Assignment:** For every severance scenario $F \in \mathcal{F}_{\text{rest}}$ ($p^w_k \cap L(F) \neq \emptyset$):
   - Construct locked slot set $S_{\text{locked}}(F)$ from all committed primaries surviving $F$ **union** all committed immutable backups $\{\pi^b_r(F)\}$ active in $F$ *(stub-released slots of severed primaries not used by existing backups in $F$ remain reusable by $k$)*.
   - Scan $p \in \mathcal{P}(s_k, d_k; G \setminus L(F), \kappa, K_{\max})$ by non-decreasing cost and $\lambda$ in **descending order** ($\lambda_{W-1} \to \lambda_0$), selecting the first pair $(p^b_k(F), \lambda^b_k(F))$ disjoint from $S_{\text{locked}}(F)$.
   - If no valid pair exists for some $F \in \mathcal{F}_{\text{rest}}$, reject $(p^w_k, \lambda^w_k)$. Otherwise commit $\pi^w_k$ and $\{\pi^b_k(F)\}_{F \in \mathcal{F}_{\text{rest}}}$.

### 3.2 `NoRerouteSeqRDF` (Sequential Immutable Baseline With Single-Pass Stage 1)
To maintain 100% methodological parity with `SeqRDF` and `GreedySeqRDF` (`EXPERIMENTS.md` Section 2.4), `NoRerouteSeqRDF` resolves Stage 1 in a **strict single pass** before verifying Stage 2:
1. **Surviving-Primary Collision Gate:** Immediately reject $(p^w_k, \lambda^w_k)$ if either:
   - $\exists x_1 \in \mathcal{E}_1$ with $p^w_k \cap L_{\text{cut}}(x_1) = \emptyset$ where $(p^w_k, \lambda^w_k)$ collides with an immutable Stage-1 backup $\pi^{s1}_r(x_1)$; or
   - $\exists F \in \mathcal{F}_{\text{seq}}$ with $p^w_k \cap L(F) = \emptyset$ where $(p^w_k, \lambda^w_k)$ collides with an immutable Stage-2 backup $\pi^{s2}_r(F)$.
2. **Phase A — Single-Pass Stage-1 Selection (All Severed First Cuts):**
   For every $x_1 \in \mathcal{E}_1$ with $p^w_k \cap L_{\text{cut}}(x_1) \neq \emptyset$:
   - Form $S_{\text{locked}}^{(1)}(x_1)$ from all committed primaries surviving $x_1$ union all immutable Stage-1 backups under $x_1$.
   - Scan $p \in \mathcal{P}(s_k, d_k; G \setminus L_{\text{cut}}(x_1), \kappa, K_{\max})$ by cost and $\lambda$ descending ($\lambda_{W-1} \to \lambda_0$), selecting the **first** clash-free Stage-1 pair $\tilde{\pi}^{s1}_k(x_1) = (\tilde{p}^{s1}_k(x_1), \tilde{\lambda}^{s1}_k(x_1))$. If none exists, reject $(p^w_k, \lambda^w_k)$ immediately.
3. **Phase B — Stage-2 Verification & Assignment:**
   - **For scenarios $F = (x_1, x_2) \in \mathcal{F}_{\text{seq}}$ where $p^w_k \cap L_{\text{cut}}(x_1) \neq \emptyset$:**
     Let $S_{\text{locked}}^{(2)}(F)$ be all committed primaries surviving $F$ union all immutable Stage-2 backups active in $F$.
     - *If $\tilde{p}^{s1}_k(x_1) \cap L_{\text{cut}}(x_2) = \emptyset$ (Stage-1 backup survives $x_2$):* $\tilde{\pi}^{s1}_k(x_1)$ is locked in Stage 2; verify that $\{(\ell, \tilde{\lambda}^{s1}_k(x_1)) \mid \ell \in \tilde{p}^{s1}_k(x_1)\} \cap S_{\text{locked}}^{(2)}(F) = \emptyset$. If a collision exists, reject $(p^w_k, \lambda^w_k)$; otherwise set $\tilde{\pi}^{s2}_k(F) = \tilde{\pi}^{s1}_k(x_1)$.
     - *If $\tilde{p}^{s1}_k(x_1) \cap L_{\text{cut}}(x_2) \neq \emptyset$ (Stage-1 backup severed by $x_2$):* Select the first clash-free Stage-2 pair $\tilde{\pi}^{s2}_k(F)$ on $G \setminus L(F)$ avoiding $S_{\text{locked}}^{(2)}(F)$ (rejecting $(p^w_k, \lambda^w_k)$ if none exists).
   - **For scenarios $F = (x_1, x_2) \in \mathcal{F}_{\text{seq}}$ where $p^w_k \cap L_{\text{cut}}(x_1) = \emptyset$ and $p^w_k \cap L_{\text{cut}}(x_2) \neq \emptyset$:**
     Select the first clash-free Stage-2 pair $\tilde{\pi}^{s2}_k(F)$ on $G \setminus L(F)$ avoiding $S_{\text{locked}}^{(2)}(F)$ (rejecting $(p^w_k, \lambda^w_k)$ if none exists).
4. **Commit:** If all required Stage-1 and Stage-2 states pass, commit $\pi^w_k$, $\{\tilde{\pi}^{s1}_k(x_1)\}$, and $\{\tilde{\pi}^{s2}_k(F)\}$.

---

## 4. Invariants & Time Complexity

- **System Invariants:** Satisfies all five System Invariants in Section 9 of `PROBLEM.md`.
- **Per-State Time Complexity:** Evaluates at most $|U| = 1$ request ($k$), requiring $O(\bar{H})$ for a locked Stage-2 collision check or $O(|\mathcal{C}| \cdot W \cdot \bar{H})$ for single-request First-Fit selection:
  $$T_{\text{state}}^{\text{NoReroute}} = O\big( |\mathcal{C}| \cdot W \cdot \bar{H} \big)$$
- **Per-Request Worst-Case Time Complexity (Both Simultaneous & Single-Pass Sequential):**
  $$T_{\text{req}}^{\text{NoReroute}} = O\Big( |\mathcal{P}^w| \cdot W \cdot \big( |\mathcal{F}_{\text{inter}}| \cdot \bar{H} + |\mathcal{F}_{\text{rest}}| \cdot |\mathcal{C}| \cdot W \cdot \bar{H} \big) \Big)$$

