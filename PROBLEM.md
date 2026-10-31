# Problem Specification: Proactive Dual-Link Failure Restoration in Optical Networks (ToN 2026)

> **Document Purpose:** This document defines the theoretical foundation, physical constraints, mathematical formulations, research objectives, and evaluation metrics for the online dual-link failure restoration problem investigated in the IEEE/ACM *Transactions on Networking* (ToN) submission `Writing/ToN_2026.tex`. It serves as the authoritative theoretical specification of **what** problem is solved, **why** it matters, and **how** network performance is quantified. The algorithmic architecture index is given in `ALGORITHMS.md` with dedicated modular heuristic specifications in `algorithms/`, and the benchmark suite, fairness protocol, and runtime budget analysis are specified in `EXPERIMENTS.md`.

---

## Table of Contents
1. [Executive Summary & Motivation](#1-executive-summary--motivation)
2. [Optical Network & Traffic Model](#2-optical-network--traffic-model)
3. [Physical Failure Cut Models & Scenario Filtering](#3-physical-failure-cut-models--scenario-filtering)
4. [Failure Timing Regimes: Simultaneous vs. Sequential](#4-failure-timing-regimes-simultaneous-vs-sequential)
5. [Resource Sharing, Multiplexing & Stub-Release](#5-resource-sharing-multiplexing--stub-release)
6. [Mathematical Formulation](#6-mathematical-formulation)
7. [Research Objectives & Paper Scope](#7-research-objectives--paper-scope)
8. [Benchmark Topologies, Traffic Profiles & Evaluation Metrics](#8-benchmark-topologies-traffic-profiles--evaluation-metrics)
9. [System Invariants for Theoretical Verification](#9-system-invariants-for-theoretical-verification)

---

## 1. Executive Summary & Motivation

In optical wavelength-division multiplexing (WDM) backbone networks supporting modern 5G/6G infrastructures, network densification significantly increases vulnerability to fiber cuts caused by civil construction, natural hazards, or equipment wear. While single-link failures have been extensively studied, **dual-link failures** represent a severe reliability bottleneck:
- A second fiber cut often occurs before the first cut can be physically repaired (the Mean Time to Repair, MTTR, for terrestrial fiber spans typically ranges from 12 to 48 hours).
- The network must remain resilient even while operating in an already degraded topology.

Existing literature largely investigates single-link failures or treats dual failures exclusively under an idealized simultaneous occurrence assumption. This paper addresses this gap by formulating a **unified, proactive online provisioning framework** that rigorously models and compares two physical timing regimes:
1. **Simultaneous Dual Failures:** Both failure events occur concurrently (or within a reaction window shorter than control-plane recovery), requiring single-stage joint restoration on $G \setminus L(F)$.
2. **Sequential Dual Failures:** Failure unit $x_1$ fails first, triggering an initial restoration phase on $G \setminus L_{\text{cut}}(x_1)$. Failure unit $x_2$ subsequently fails before $x_1$ is repaired, triggering a second restoration phase on $G \setminus \big(L_{\text{cut}}(x_1) \cup L_{\text{cut}}(x_2)\big)$ subject to operational continuity of surviving Stage-1 restoration lightpaths.

### Core Goal
Given an optical transport topology and an online sequence of permanent connection requests, design a proactive provisioning framework that **guarantees 100% restorability** across all admissible non-isolating dual-link failure scenarios while maximizing admitted network throughput and minimizing protection spectrum overhead.

---

## 2. Optical Network & Traffic Model

### 2.1 Optical Physical Infrastructure
The optical transport network is represented by a symmetric directed graph $G = (V, L)$ with an associated undirected physical span set $E$:
- $V$ is the set of optical routing nodes (Reconfigurable Optical Add-Drop Multiplexers, ROADMs), with $|V| = N$.
- $L$ is the set of directed fiber links (arcs), with $|L| = M$.
- $E$ is the set of bidirectional physical fiber conduits (spans). Each span $e = \{u, v\} \in E$ houses counter-propagating fibers represented by two directed arcs $(u, v) \in L$ and $(v, u) \in L$ (so $|E| = M / 2$).
- Each directed link $\ell \in L$ supports a discrete set of orthogonal wavelength channels $\Lambda = \{\lambda_0, \lambda_1, \dots, \lambda_{W-1}\}$ of cardinality $|\Lambda| = W$.
- Nodes operate **without wavelength converters** (all-optical O-O-O switching is enforced to preserve ultra-low latency, protocol transparency, and energy efficiency).

### 2.2 Optical Transmission Constraints
Every active lightpath $\pi = (p, \lambda)$—consisting of a simple directed route $p \subseteq L$ and an assigned wavelength channel $\lambda \in \Lambda$—must satisfy two fundamental physical constraints under any network operational state:
1. **Wavelength Continuity Constraint:** A lightpath must occupy the identical optical wavelength channel $\lambda$ on every directed link along its route:
   $$\forall \ell \in p, \quad \lambda(\pi, \ell) = \lambda$$
2. **Wavelength Clash Constraint:** No two distinct lightpaths $\pi_1 = (p_1, \lambda_1)$ and $\pi_2 = (p_2, \lambda_2)$ that are simultaneously active in the same operational state may share the same wavelength channel on any common directed link:
   $$\forall \pi_1 \neq \pi_2 \text{ active concurrently}, \quad (p_1 \cap p_2 \neq \emptyset) \implies \lambda_1 \neq \lambda_2$$

### 2.3 Incremental Online Traffic Model
- Connection requests arrive sequentially in an online stream: $k_0, k_1, \dots, k_{T-1}$, without prior knowledge of future arrivals.
- Traffic is incremental (permanent within an evaluation sequence; admitted lightpaths are not torn down).
- Each request $k_t = (s_{k_t}, d_{k_t})$ with $s_{k_t} \neq d_{k_t}$ demands 1 full wavelength channel from source $s_{k_t} \in V$ to destination $d_{k_t} \in V$.
- **Strict Admissibility Policy:** Upon arrival of request $k$:
  - **Granted:** The system commits a clash-free primary (working) lightpath $\pi^w_k = (p^w_k, \lambda^w_k)$ and guarantees valid, clash-free restoration backup lightpaths for every admissible failure state that disrupts $k$ (and maintains valid restoration lightpaths for all previously admitted requests across all admissible failure states).
  - **Denied (Blocked):** If 100% restorability across all admissible failure states cannot be guaranteed within link capacity $W$ and path stretch bound $\kappa$, request $k$ is immediately rejected without altering the network state.
- **Lightpath Mutability Rules:**
  - **Primary lightpaths are strictly immutable:** Once request $r$ is admitted, its working route and wavelength $\pi^w_r = (p^w_r, \lambda^w_r)$ cannot be rerouted or recolored, preventing service disruption to live traffic.
  - **Backup reservations are dynamically reconfigurable:** Because backup lightpaths exist only as control-plane reservations while the network is healthy, previously planned backup routes and wavelengths for existing requests may be dynamically reconfigured upon admitting a new request—provided 100% restorability across all failure scenarios is preserved.

### 2.4 Bidirectional Spectral Assignment Strategy
To mitigate spectral fragmentation between immutable working lightpaths and reconfigurable protection reservations:
- **Primary lightpaths** evaluate wavelengths in **ascending order** ($\lambda_0, \lambda_1, \dots, \lambda_{W-1}$).
- **Restoration backup lightpaths** favor wavelengths toward the **upper end of the spectrum** ($\lambda_{W-1}, \lambda_{W-2}, \dots, \lambda_0$).

This bidirectional partitioning creates an adaptive spectral guard band between working and protection allocations, delaying cross-regime contention until high network loads.

---

## 3. Physical Failure Cut Models & Scenario Filtering

### 3.1 Physical Cut Models
Let $\mathcal{I}$ denote the set of elementary physical failure units, and let $L_{\text{cut}}(x) \subseteq L$ denote the set of directed arcs severed when unit $x \in \mathcal{I}$ fails. The framework models two physical failure granularities:

1. **Standard Physical Span Cuts (Bidirectional Conduit Cuts, $\mathcal{I} = E$):**
   A backhoe cut or cable severance cuts an entire physical conduit $e = \{u, v\} \in E$, simultaneously severing both counter-propagating directions:
   $$L_{\text{cut}}(e) = \{(u, v), (v, u)\} \cap L$$
   A dual-span failure combining distinct spans $e_1, e_2 \in E$ severs $L(F) = L_{\text{cut}}(e_1) \cup L_{\text{cut}}(e_2)$ (4 directed arcs).

2. **Directional Arc Cuts (Unidirectional Fiber/Amplifier Cuts, $\mathcal{I} = L$):**
   Individual fiber cores, line cards, or unidirectional optical amplifiers fail independently. Each unit is a single directed arc $\ell \in L$:
   $$L_{\text{cut}}(\ell) = \{\ell\}$$
   A dual-arc failure combining distinct arcs $\ell_1, \ell_2 \in L$ severs $L(F) = \{\ell_1, \ell_2\}$ (2 directed arcs), leaving counter-propagating arcs operational unless explicitly cut.

### 3.2 Non-Isolating Cutset Filtering
If removing the severed arcs $L(F)$ partitions $G$ such that the surviving digraph $G \setminus L(F)$ is no longer strongly connected, demands crossing disconnected components are topologically unrecoverable by any routing or coloring algorithm. Such topology-disconnecting cutsets are excluded during scenario enumeration:

- **Simultaneous Scenario Space (Unordered Pairs):**
  $$\mathcal{F}_{\text{sim}} = \left\{ F = \{x_1, x_2\} \in \binom{\mathcal{I}}{2} \;\middle|\; G \setminus \big(L_{\text{cut}}(x_1) \cup L_{\text{cut}}(x_2)\big) \text{ is strongly connected} \right\}$$
- **Sequential Scenario Space (Ordered Pairs):**
  $$\mathcal{F}_{\text{seq}} = \Big\{ F = (x_1, x_2) \in \mathcal{I} \times \mathcal{I} \;\Big|\; x_1 \neq x_2 \text{ and } G \setminus \big(L_{\text{cut}}(x_1) \cup L_{\text{cut}}(x_2)\big) \text{ is strongly connected} \Big\}$$

Note that $|\mathcal{F}_{\text{seq}}| = 2 |\mathcal{F}_{\text{sim}}|$, and because $L_{\text{cut}}(x_1) \subset L_{\text{cut}}(x_1) \cup L_{\text{cut}}(x_2)$, strong connectivity of $G \setminus L((x_1, x_2))$ automatically ensures that the Stage-1 intermediate topology $G \setminus L_{\text{cut}}(x_1)$ is strongly connected for every active first-stage failure unit $x_1 \in \mathcal{E}_1 = \{ x_1 \in \mathcal{I} \mid \exists x_2 : (x_1, x_2) \in \mathcal{F}_{\text{seq}} \}$.

**Benchmark Non-Isolating Scenario Counts:**

| Topology | Nodes $\|V\|$ | Directed Links $\|L\|$ (Spans $\|E\|$) | Standard Span Cuts ($\|\mathcal{F}_{\text{sim}}\|$ / $\|\mathcal{F}_{\text{seq}}\|$) | Directional Arc Cuts ($\|\mathcal{F}_{\text{sim}}\|$ / $\|\mathcal{F}_{\text{seq}}\|$) |
| :--- | :---: | :---: | :---: | :---: |
| **Nobel-EU** | 28 | 82 (41 spans) | 804 / 1,608 | 3,289 / 6,578 |
| **Germany50** | 50 | 176 (88 spans) | 3,817 / 7,634 | 15,378 / 30,756 |

---

## 4. Failure Timing Regimes: Simultaneous vs. Sequential

```mermaid
flowchart TD
    subgraph Simultaneous["Simultaneous Regime: Unordered Pair {x1, x2}"]
        S0["Normal State G"] -->|"x1 and x2 fail concurrently"| S1["Degraded Topology G \ L({x1, x2})"]
        S1 -->|"Single-Stage Restoration"| S2["All severed primaries restored jointly on G \ L({x1, x2})"]
    end

    subgraph Sequential["Sequential Regime: Ordered Sequence (x1, x2)"]
        Q0["Normal State G"] -->|"x1 fails first"| Q1["Stage-1 Topology G \ L_cut(x1) (x2 still healthy)"]
        Q1 -->|"Stage-1 Restoration"| Q2["Primaries cut by x1 restored on G \ L_cut(x1)"]
        Q2 -->|"x2 fails before x1 is repaired"| Q3["Stage-2 Topology G \ L((x1, x2))"]
        Q3 -->|"Stage-2 Restoration"| Q4["Surviving Stage-1 backups stay FIXED; severed Stage-1 backups and primaries cut by x2 restored"]
    end
```

### 4.1 Fundamental Operational Differences
1. **Availability of $x_2$ During Stage 1:**
   - Under simultaneous failure $\{x_1, x_2\}$, unit $x_2$ is already down at the moment of recovery and can never carry restoration traffic.
   - Under sequential failure $(x_1, x_2)$, unit $x_2$ is **healthy and operational** when $x_1$ fails; Stage-1 backup lightpaths on $G \setminus L_{\text{cut}}(x_1)$ cannot predict which link will fail second and may legitimately traverse $L_{\text{cut}}(x_2)$.
2. **Failure Ordering & First-Cut Coupling:**
   - Simultaneous scenarios are independent unordered sets $\{x_1, x_2\} = \{x_2, x_1\} \in \mathcal{F}_{\text{sim}}$.
   - Sequential scenarios are ordered pairs $(x_1, x_2) \neq (x_2, x_1) \in \mathcal{F}_{\text{seq}}$. Because the control plane observes only $x_1$ during Stage 1, all sequential scenarios sharing the same first failure unit $x_1$ form a **coupled scenario group**:
     $$\mathcal{G}_{x_1} = \big\{ (x_1, x_2) \in \mathcal{F}_{\text{seq}} \big\}$$
     Every scenario in $\mathcal{G}_{x_1}$ **must share the exact same Stage-1 backup allocation** $\{\pi^{s1}_r(x_1)\}$ for requests severed by $x_1$.
3. **Stage-2 Fixed Allocation Invariant (No Disruption of Healthy Active Backups):**
   - When $x_2$ subsequently fails, any request $r$ whose primary was severed by $x_1$ and whose active Stage-1 backup route $p^{s1}_r(x_1)$ **avoids $L_{\text{cut}}(x_2)$** continues carrying live restored traffic without interruption. Its lightpath $\pi^{s1}_r(x_1)$ is **locked and immutable** throughout Stage 2.
   - Only requests in two disrupted categories undergo Stage-2 restoration on $G \setminus L((x_1, x_2))$:
     1. Requests severed by $x_1$ whose Stage-1 backup route traverses $L_{\text{cut}}(x_2)$ (double-disrupted requests); and
     2. Requests whose primary route avoided $L_{\text{cut}}(x_1)$ and is newly severed by $L_{\text{cut}}(x_2)$.

---

## 5. Resource Sharing, Multiplexing & Stub-Release

### 5.1 Shared Backup Multiplexing Across Mutually Exclusive Failures
Backup lightpaths remain unlit reservations during normal operation. Because distinct failure events $F_1 \neq F_2$ do not occur simultaneously as separate network states, backup lightpaths planned for different failure scenarios may freely multiplex onto the exact same link-wavelength slot $(\ell, \lambda)$. Clash-freedom is enforced strictly among lightpaths active within the **same** operational state.

### 5.2 Stub-Release Mechanics (Failure-Dependent Primary Slot Recycling)
When a fiber cut severs an active lightpath, fast optical link-down signaling extinguishes laser transmission along that lightpath. Consequently, the wavelength channels formerly occupied by the disrupted lightpath on **surviving links upstream and downstream of the cut** become dark and available for restoration:
- **Primary Stub-Release Rule:** In any degraded state with severed arc set $L_{\text{fail}}$ (whether Stage 1 with $L_{\text{fail}} = L_{\text{cut}}(x_1)$ or dual failure with $L_{\text{fail}} = L(F)$), if an admitted request $r$'s primary route satisfies $p^w_r \cap L_{\text{fail}} \neq \emptyset$, then all surviving primary slots $\big\{ (\ell, \lambda^w_r) \;\big|\; \ell \in p^w_r \setminus L_{\text{fail}} \big\}$ are immediately eligible for reuse by backup lightpaths in that state.
- **Stage-1 Backup Stub-Release Rule (Sequential Stage 2):** Similarly, under sequential scenario $F = (x_1, x_2)$, if a request $r$'s Stage-1 backup route $p^{s1}_r(x_1)$ is severed by $x_2$ ($p^{s1}_r(x_1) \cap L_{\text{cut}}(x_2) \neq \emptyset$), its Stage-1 backup transmission is extinguished and its surviving Stage-1 slots $\big\{ (\ell, \lambda^{s1}_r(x_1)) \;\big|\; \ell \in p^{s1}_r(x_1) \setminus L(F) \big\}$ are released for Stage-2 reuse.

### 5.3 Protected Surviving Primary Slots
Conversely, if an admitted request $r$'s primary route does **not** intersect the severed arc set ($p^w_r \cap L_{\text{fail}} = \emptyset$), $\pi^w_r$ remains live throughout that failure state, and all of its slots $\big\{ (\ell, \lambda^w_r) \;\big|\; \ell \in p^w_r \big\}$ are strictly excluded from backup assignment in that state.

---

## 6. Mathematical Formulation

### 6.1 Notation Reference Table

| Symbol | Definition | Domain / Type |
| :--- | :--- | :--- |
| $G = (V, L), \; E$ | Directed optical network graph ($N = \|V\|, M = \|L\|$) and undirected span set ($\|E\| = M/2$) | Graph / Sets |
| $\Lambda, \; W$ | Discrete set of optical wavelengths and link capacity ($W = \|\Lambda\|$) | Finite set, integer |
| $c(\ell), \; c(p)$ | Configurable positive cost of link $\ell \in L$ and additive cost of path $p$ ($\sum_{\ell \in p} c(\ell)$) | Positive real ($\mathbb{R}_{>0}$) |
| $k_t = (s_{k_t}, d_{k_t})$ | $t$-th online unit-wavelength connection request ($t \in \{0, \dots, T-1\}$) | Node pair ($s \neq d$) |
| $K_{\text{adm}}(t)$ | Set of admitted requests after processing request $k_t$ | Request subset |
| $\pi^w_r = (p^w_r, \lambda^w_r)$ | Immutable primary lightpath assigned to admitted request $r$ | Route and wavelength |
| $\mathcal{I}, \; L_{\text{cut}}(x)$ | Elementary failure unit set ($E$ or $L$) and arcs severed by $x \in \mathcal{I}$ | Failure domain |
| $\mathcal{F}_{\text{sim}}, \; \mathcal{F}_{\text{seq}}$ | Non-isolating unordered (simultaneous) and ordered (sequential) dual-failure scenario sets | Scenario sets |
| $L(F)$ | Total set of directed arcs severed by dual-failure scenario $F$ | Subset of $L$ |
| $\mathcal{E}_1, \; \mathcal{G}_{x_1}$ | Active first-cut units and coupled sequential scenario group sharing first cut $x_1 \in \mathcal{E}_1$ | Failure subsets |
| $\pi^b_r(F) = (p^b_r(F), \lambda^b_r(F))$ | Backup lightpath for disrupted request $r$ under simultaneous scenario $F \in \mathcal{F}_{\text{sim}}$ | Route and wavelength |
| $\pi^{s1}_r(x_1), \; \pi^{s2}_r(F)$ | Stage-1 and Stage-2 backup lightpaths for disrupted request $r$ under $x_1 \in \mathcal{E}_1$ and $F \in \mathcal{F}_{\text{seq}}$ | Route and wavelength |
| $\kappa, \; K_{\max}$ | Maximum relative path-stretch factor and polynomial candidate-route cap | Real ($\kappa \ge 0$), integer ($K_{\max} \ge 1$) |

### 6.2 Configurable Distance Metric & Surviving-Topology Path Stretch ($\kappa, K_{\max}$)
Each directed link $\ell = (u, v) \in L$ is assigned a positive metric weight $c(\ell) > 0$ according to a configurable distance model:
1. **Geographic Fiber Distance (Physical Mode):** $c(\ell)$ equals the great-circle (Haversine) distance in kilometers computed from the geographic longitude and latitude coordinates of endpoint nodes $u$ and $v$, capturing physical propagation delay and optical attenuation reach.
2. **Topological Hop Count (Unweighted Mode):** $c(\ell) = 1$ for all $\ell \in L$, measuring route length purely in optical hops.

For any operational subgraph $G' = (V, L')$—where $G' = G$ for primary routing, $G' = G \setminus L_{\text{cut}}(x_1)$ for Stage-1 backup routing, and $G' = G \setminus L(F)$ for simultaneous or Stage-2 backup routing—let $d_{G'}(s, d)$ denote the shortest-path cost from $s$ to $d$ in $G'$. Every candidate lightpath route between $s$ and $d$ on $G'$ belongs to the $(\kappa, K_{\max})$-bounded candidate path set:
$$\mathcal{P}(s, d; G', \kappa, K_{\max}) = \Big\{ \text{up to the } K_{\max} \text{ lowest-cost simple directed } (s, d)\text{-paths } p \subseteq L' \text{ satisfying } c(p) \le (1 + \kappa) \cdot d_{G'}(s, d) \Big\}$$
enumerated via Yen's $K$-shortest simple paths in non-decreasing order of $c(p)$ (with deterministic tie-breaking). Bounding backup paths relative to the **surviving** shortest-path distance $d_{G'}(s, d)$ ensures non-empty detour sets whenever $G'$ is strongly connected, while capping enumeration at $K_{\max}$ guarantees strict worst-case polynomial complexity even at larger stretch values $\kappa$.

---

## 7. Research Objectives & Paper Scope

The study targets four principal research questions:

1. **Simultaneous vs. Sequential Failure Dynamics:**
   How does the physical failure timing assumption—unordered simultaneous recovery versus two-stage sequential recovery with coupled Stage-1 allocations and locked surviving backups—impact network capacity exhaustion, first denial threshold, blocking probability, and protection spectrum overhead on identical topologies?
2. **Value of Dynamic Backup Reconfiguration:**
   What throughput and spectral-efficiency gains are achieved by dynamically reconfiguring existing backup reservations upon new arrivals compared against an immutable-backup baseline (`No-Reroute`)?
3. **Domain-Adapted Graph Coloring vs. Greedy First-Fit:**
   How substantially does a contention- and stability-aware DSATUR graph-coloring heuristic outperform static-order Greedy First-Fit in request acceptance, blocking rate, and spectral compaction—particularly under realistic hub-skewed traffic and dense conflict topologies?
4. **Directional Arc Cuts vs. Bidirectional Span Cuts:**
   How does failure cut granularity (unidirectional fiber/amplifier cuts versus full bidirectional conduit cuts) alter scenario multiplicity, surviving routing diversity, stub-release recycling, and spectral contention?

---

## 8. Benchmark Topologies, Traffic Profiles & Evaluation Metrics

### 8.1 Benchmark Topologies
Experiments are evaluated on two standard European transport topologies from SNDlib:
- **Nobel-EU:** $N = 28$ nodes, $|E| = 41$ physical spans ($M = 82$ directed arcs), $W = 30$ wavelengths per arc.
- **Germany50:** $N = 50$ nodes, $|E| = 88$ physical spans ($M = 176$ directed arcs), $W = 30$ wavelengths per arc.
- Routing configuration: Geographic fiber distance (Haversine km from SNDlib node coordinates, configurable to hop count), stretch bound $\kappa = 0.10$, polynomial candidate cap $K_{\max} = 10$.

### 8.2 Symmetric Online Traffic Profiles Across Both Topologies
Both topologies are evaluated under symmetric demand models of $T = 1,000$ permanent unit-wavelength requests per run:
1. **Realistic SNDlib Demand-Weighted Traffic:** Requests $(s, d)$ sampled with probability proportional to empirical SNDlib traffic demand intensity $D(s, d)$:
   - **Germany50 (`1d1m` DFN Archive):** $28$ independent runs corresponding to the $28$ daily measured DFN backbone demand matrices.
   - **Nobel-EU (SNDlib Empirical Demand Matrix):** $28$ independent random runs sampled proportional to SNDlib's official European city-pair static demand matrix for `nobel-eu`.
2. **Uniform Dynamic Traffic:** $28$ independent random runs on both **Nobel-EU** and **Germany50** sampled uniformly over ordered node pairs $(s, d) \in V \times V$ ($s \neq d$), isolating pure topological bottleneck structure from hub-traffic skewness.

### 8.3 Request-Level & Step-Indexed Evaluation Metrics

#### A. Throughput, Blocking & Computational Effort Metrics
1. **First Denial Request Index ($I_{\text{denial}}$):** The 0-based stream index $t \in \{0, \dots, T-1\}$ of the first rejected request (set to $T$ if no request is denied), measuring lossless capacity headroom under strict SLA guarantees.
2. **Total Granted Requests ($K_{\text{granted}} = |K_{\text{adm}}(T-1)|$) & Blocking Probability ($B = 1 - K_{\text{granted}} / T$):** Cumulative network throughput and rejection rate over the $T$-request stream.
3. **Provisioning Latency ($T_{\text{req}}(t)$ and $T_{\text{avg}}$):** Per-request decision time recorded after every arrival $t$, summarized both up to first denial ($t < I_{\text{denial}}$) and across the complete $T$-request sequence.

#### B. Step-Indexed Spectral Utilization Metrics (Recorded After Every Request $t = 0, \dots, T-1$)
After each request $k_t$ is processed (granted or denied), let $K_{\text{adm}}(t)$ be the current set of admitted requests. Across the total network capacity of $M \times W$ directed link-wavelength slots $(\ell, \lambda) \in L \times \Lambda$, define the occupied slot sets at step $t$:

1. **Primary-Occupied Slot Set $H_{\text{prim}}(t) \subseteq L \times \Lambda$:**
   The set of slots carrying active working lightpaths under normal operation:
   $$H_{\text{prim}}(t) = \bigcup_{r \in K_{\text{adm}}(t)} \big\{ (\ell, \lambda^w_r) \;\big|\; \ell \in p^w_r \big\}$$

2. **Backup-Reserved Slot Set $H_{\text{back}}(t) \subseteq L \times \Lambda$:**
   The set of slots reserved by **at least one** restoration lightpath across all admissible failure states:
   - *Simultaneous Regime:*
     $$H_{\text{back}}(t) = \bigcup_{F \in \mathcal{F}_{\text{sim}}} \; \bigcup_{\substack{r \in K_{\text{adm}}(t) \\ p^w_r \cap L(F) \neq \emptyset}} \big\{ (\ell, \lambda^b_r(F)) \;\big|\; \ell \in p^b_r(F) \big\}$$
   - *Sequential Regime (union over Stage-1 and Stage-2 reservations):*
     $$H_{\text{back}}(t) = \left( \bigcup_{x_1 \in \mathcal{E}_1} \; \bigcup_{\substack{r \in K_{\text{adm}}(t) \\ p^w_r \cap L_{\text{cut}}(x_1) \neq \emptyset}} \big\{ (\ell, \lambda^{s1}_r(x_1)) \;\big|\; \ell \in p^{s1}_r(x_1) \big\} \right) \cup \left( \bigcup_{F \in \mathcal{F}_{\text{seq}}} \; \bigcup_{\substack{r \in K_{\text{adm}}(t) \\ p^w_r \cap L(F) \neq \emptyset}} \big\{ (\ell, \lambda^{s2}_r(F)) \;\big|\; \ell \in p^{s2}_r(F) \big\} \right)$$

Because Stub-Release allows backup lightpaths in failure scenarios to recycle surviving links of severed primaries, $H_{\text{prim}}(t)$ and $H_{\text{back}}(t)$ overlap on **stub-reused (dual-use) slots** $H_{\text{stub}}(t) = H_{\text{prim}}(t) \cap H_{\text{back}}(t)$, while **pure protection (dark backup) slots** are given by the set difference:
$$H_{\text{pure}}(t) = H_{\text{back}}(t) \setminus H_{\text{prim}}(t)$$

At every request index $t \in \{0, 1, \dots, T-1\}$, the system records the following spectral metrics:
- **Primary Capacity Utilization:**
  $$U_{\text{prim}}(t) = \frac{|H_{\text{prim}}(t)|}{M \times W} \times 100\%$$
- **Total Backup Footprint Utilization** *(all slots touched by any backup reservation, including multiplexed and stub-reused slots)*:
  $$U_{\text{back}}(t) = \frac{|H_{\text{back}}(t)|}{M \times W} \times 100\%$$
- **Pure Backup (Dark Protection) Utilization** *(dedicated protection capacity reserved exclusively outside working slots)*:
  $$U_{\text{pure}}(t) = \frac{|H_{\text{pure}}(t)|}{M \times W} = \frac{|H_{\text{back}}(t) \setminus H_{\text{prim}}(t)|}{M \times W} \times 100\%$$
- **Stub-Release Recycled Utilization** *(working spectrum simultaneously recycled for restoration)*:
  $$U_{\text{stub}}(t) = \frac{|H_{\text{prim}}(t) \cap H_{\text{back}}(t)|}{M \times W} \times 100\% = U_{\text{back}}(t) - U_{\text{pure}}(t)$$
- **Total Network Spectral Footprint:**
  $$U_{\text{total}}(t) = \frac{|H_{\text{prim}}(t) \cup H_{\text{back}}(t)|}{M \times W} \times 100\% = U_{\text{prim}}(t) + U_{\text{pure}}(t)$$
- **CapEx Protection Overhead Ratio** *(for $|H_{\text{prim}}(t)| > 0$, quantifying extra dedicated protection slots per working slot)*:
  $$\rho_{\text{overhead}}(t) = \frac{|H_{\text{pure}}(t)|}{|H_{\text{prim}}(t)|} = \frac{|H_{\text{back}}(t) \setminus H_{\text{prim}}(t)|}{|H_{\text{prim}}(t)|}$$

Recording this complete vector after every request $t$ enables plotting continuous evolution curves over $t \in [0, T-1]$ as well as extracting exact point comparisons at $t = I_{\text{denial}} - 1$ (peak lossless load) and $t = T - 1$ (saturated steady state).

---

## 9. System Invariants for Theoretical Verification

Every valid provisioning state after any request step $t$ must satisfy five hard invariants:
1. **Primary Clash Conservation:** No two admitted requests $r_1 \neq r_2 \in K_{\text{adm}}(t)$ occupy a common slot $(\ell, \lambda)$ along their primary lightpaths.
2. **100% Dual-Failure Survivability:** For every admitted request $r \in K_{\text{adm}}(t)$ and every admissible failure state (every $F \in \mathcal{F}_{\text{sim}}$ under simultaneous mode; every $x_1 \in \mathcal{E}_1$ and every $F \in \mathcal{F}_{\text{seq}}$ under sequential mode), $r$ possesses a valid, $\kappa$-bounded, wavelength-continuous, clash-free active lightpath (its surviving primary or its assigned backup) on the corresponding surviving graph.
3. **Sequential Stage-1 Isolation & Coupling:** During Stage-1 restoration for failure unit $x_1$, every other unit $x_2 \neq x_1$ remains operational, and all scenarios $(x_1, x_2) \in \mathcal{G}_{x_1}$ share the identical Stage-1 allocation $\{\pi^{s1}_r(x_1)\}$.
4. **Sequential Stage-2 Fixed Allocation Invariant:** For every $(x_1, x_2) \in \mathcal{F}_{\text{seq}}$ and every $r \in K_{\text{adm}}(t)$ severed by $x_1$, if $p^{s1}_r(x_1) \cap L_{\text{cut}}(x_2) = \emptyset$, then $\pi^{s2}_r((x_1, x_2)) = \pi^{s1}_r(x_1)$ holds without alteration.
5. **Exact Stub-Release Boundary:** Within any failure state $L_{\text{fail}}$, a backup lightpath may occupy a primary slot $(\ell, \lambda^w_r)$ if and only if $p^w_r \cap L_{\text{fail}} \neq \emptyset$ and $\ell \notin L_{\text{fail}}$.
