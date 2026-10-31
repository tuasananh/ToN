#include "ton/core/scenarios.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace ton {

namespace {

constexpr size_t kMaxStackNodes = 128;

template <typename BufferInt, typename BufferBool, typename BufferFrame>
bool tarjan_single_scc_impl(const Graph& graph, const CutArcs& cut_arcs,
                            BufferInt& disc, BufferInt& low,
                            BufferBool& on_stack, BufferInt& scc_stack,
                            BufferFrame& call_stack) noexcept {
  const NodeId n = graph.num_nodes();
  if (n <= 1) {
    return true;
  }

  int32_t timer = 0;
  size_t scc_top = 0;
  size_t call_top = 0;

  disc[0] = timer;
  low[0] = timer;
  ++timer;
  on_stack[0] = 1;
  scc_stack[scc_top++] = 0;
  call_stack[call_top++] = std::pair<NodeId, uint16_t>{0, 0};

  while (call_top > 0) {
    auto& [u, edge_idx] = call_stack[call_top - 1];
    const auto out = graph.out_arcs(u);

    if (edge_idx < out.size()) {
      const ArcId a = out[edge_idx++];
      if (cut_arcs.contains(a)) {
        continue;
      }
      const NodeId v = graph.arc(a).dst;
      if (disc[v] == -1) {
        disc[v] = timer;
        low[v] = timer;
        ++timer;
        on_stack[v] = 1;
        scc_stack[scc_top++] = static_cast<int32_t>(v);
        call_stack[call_top++] = std::pair<NodeId, uint16_t>{v, 0};
      } else if (on_stack[v] != 0) {
        low[u] = std::min(low[u], disc[v]);
      }
    } else {
      --call_top;
      if (call_top > 0) {
        const NodeId parent = call_stack[call_top - 1].first;
        low[parent] = std::min(low[parent], low[u]);
      }
      if (low[u] == disc[u]) {
        uint32_t scc_size = 0;
        while (scc_top > 0) {
          const auto w = static_cast<NodeId>(scc_stack[--scc_top]);
          on_stack[w] = 0;
          ++scc_size;
          if (w == u) {
            break;
          }
        }
        // Strongly connected iff the very first completed SCC contains all N
        // nodes.
        return scc_size == n;
      }
    }
  }
  return false;
}

void verify_canonical_counts(std::string_view topo_name, CutModel model,
                             size_t sim_count, size_t seq_count) {
  auto check_exact = [&](size_t exp_sim, size_t exp_seq) {
    if (sim_count != exp_sim || seq_count != exp_seq) {
      throw std::logic_error(
          "Scenario count assertion failed for '" + std::string(topo_name) +
          "' (" + (model == CutModel::Span ? "Span" : "Directional") +
          "): got " + std::to_string(sim_count) + " / " +
          std::to_string(seq_count) + ", expected " + std::to_string(exp_sim) +
          " / " + std::to_string(exp_seq));
    }
  };

  if (topo_name == "nobel-eu") {
    if (model == CutModel::Span) {
      check_exact(804, 1608);
    } else {
      check_exact(3289, 6578);
    }
  } else if (topo_name == "germany50") {
    if (model == CutModel::Span) {
      check_exact(3817, 7634);
    } else {
      check_exact(15378, 30756);
    }
  }
}

}  // namespace

bool is_strongly_connected(const Graph& graph, const CutArcs& cut_arcs) {
  const size_t n = graph.num_nodes();
  if (n <= kMaxStackNodes) {
    std::array<int32_t, kMaxStackNodes> disc;
    std::array<int32_t, kMaxStackNodes> low;
    std::array<uint8_t, kMaxStackNodes> on_stack;
    std::array<int32_t, kMaxStackNodes> scc_stack;
    std::array<std::pair<NodeId, uint16_t>, kMaxStackNodes> call_stack;
    std::fill_n(disc.begin(), n, -1);
    std::fill_n(on_stack.begin(), n, uint8_t{0});
    return tarjan_single_scc_impl(graph, cut_arcs, disc, low, on_stack,
                                  scc_stack, call_stack);
  }

  std::vector<int32_t> disc(n, -1);
  std::vector<int32_t> low(n, 0);
  std::vector<uint8_t> on_stack(n, 0);
  std::vector<int32_t> scc_stack(n, 0);
  std::vector<std::pair<NodeId, uint16_t>> call_stack(n);
  return tarjan_single_scc_impl(graph, cut_arcs, disc, low, on_stack, scc_stack,
                                call_stack);
}

ScenarioSet ScenarioSet::enumerate(const Graph& graph, CutModel cut_model,
                                   bool enforce_benchmark_assertions) {
  ScenarioSet result;
  result.cut_model_ = cut_model;
  result.num_units_ =
      cut_model == CutModel::Span ? graph.num_spans() : graph.num_arcs();

  result.unit_cuts_.resize(result.num_units_);
  result.arc_to_unit_.assign(graph.num_arcs(), INVALID_FAILURE_UNIT);

  if (cut_model == CutModel::Span) {
    for (SpanId s = 0; s < graph.num_spans(); ++s) {
      const auto& sp = graph.span(s);
      result.unit_cuts_[s].add_unique(sp.arc_uv);
      result.unit_cuts_[s].add_unique(sp.arc_vu);
      result.arc_to_unit_[sp.arc_uv] = s;
      result.arc_to_unit_[sp.arc_vu] = s;
    }
  } else {
    for (ArcId a = 0; a < graph.num_arcs(); ++a) {
      result.unit_cuts_[a].add_unique(a);
      result.arc_to_unit_[a] = a;
    }
  }

  const size_t u_count = result.num_units_;
  const size_t max_unordered = u_count * (u_count > 0 ? u_count - 1 : 0) / 2;
  result.sim_scenarios_.reserve(max_unordered);

  // Temp lookup matrix mapping unordered (x1, x2) -> SimScenarioId (or
  // INVALID_SCENARIO).
  std::vector<ScenarioId> pair_to_sim(u_count * u_count, INVALID_SCENARIO);

  for (FailureUnitId x1 = 0; x1 < result.num_units_; ++x1) {
    for (FailureUnitId x2 = static_cast<FailureUnitId>(x1 + 1);
         x2 < result.num_units_; ++x2) {
      CutArcs combined = result.unit_cuts_[x1];
      for (const ArcId a : result.unit_cuts_[x2].span()) {
        combined.add_unique(a);
      }
      if (!is_strongly_connected(graph, combined)) {
        continue;
      }
      const auto sim_id = static_cast<ScenarioId>(result.sim_scenarios_.size());
      result.sim_scenarios_.push_back(SimScenario{
          .id = sim_id,
          .x1 = x1,
          .x2 = x2,
          .cut_arcs = combined,
      });
      pair_to_sim[static_cast<size_t>(x1) * u_count + x2] = sim_id;
      pair_to_sim[static_cast<size_t>(x2) * u_count + x1] = sim_id;
    }
  }

  // Enumerate ordered sequential pairs grouped contiguously by first failure
  // unit x1.
  result.seq_scenarios_.reserve(result.sim_scenarios_.size() * 2);
  result.unit_is_active_s1_.assign(u_count, 0);
  result.seq_group_ranges_.assign(u_count, {0u, 0u});

  for (FailureUnitId x1 = 0; x1 < result.num_units_; ++x1) {
    const auto group_start =
        static_cast<uint32_t>(result.seq_scenarios_.size());
    for (FailureUnitId x2 = 0; x2 < result.num_units_; ++x2) {
      if (x1 == x2) {
        continue;
      }
      const ScenarioId sim_id =
          pair_to_sim[static_cast<size_t>(x1) * u_count + x2];
      if (sim_id == INVALID_SCENARIO) {
        continue;
      }
      const auto seq_id = static_cast<ScenarioId>(result.seq_scenarios_.size());
      result.seq_scenarios_.push_back(SeqScenario{
          .id = seq_id,
          .x1 = x1,
          .x2 = x2,
          .sim_id = sim_id,
          .stage1_cut = result.unit_cuts_[x1],
          .stage2_cut = result.unit_cuts_[x2],
          .total_cut = result.sim_scenarios_[sim_id].cut_arcs,
      });
    }
    const auto group_len =
        static_cast<uint32_t>(result.seq_scenarios_.size()) - group_start;
    result.seq_group_ranges_[x1] = {group_start, group_len};
    if (group_len > 0) {
      result.unit_is_active_s1_[x1] = 1;
      result.active_stage1_units_.push_back(x1);
    }
  }

  // Build CSR reverse incidence tables: ArcId -> affected SimScenarioIds &
  // Stage-2 SeqScenarioIds.
  const size_t m = graph.num_arcs();
  std::vector<std::vector<ScenarioId>> per_arc_sim(m);
  for (const auto& sc : result.sim_scenarios_) {
    for (const ArcId a : sc.cut_arcs.span()) {
      per_arc_sim[a].push_back(sc.id);
    }
  }
  result.arc_sim_offsets_.resize(m + 1, 0);
  for (size_t a = 0; a < m; ++a) {
    result.arc_sim_offsets_[a] =
        static_cast<uint32_t>(result.arc_sim_ids_.size());
    result.arc_sim_ids_.insert(result.arc_sim_ids_.end(),
                               per_arc_sim[a].begin(), per_arc_sim[a].end());
  }
  result.arc_sim_offsets_[m] =
      static_cast<uint32_t>(result.arc_sim_ids_.size());

  std::vector<std::vector<ScenarioId>> per_arc_seq_s2(m);
  for (const auto& sc : result.seq_scenarios_) {
    for (const ArcId a : sc.stage2_cut.span()) {
      per_arc_seq_s2[a].push_back(sc.id);
    }
  }
  result.arc_seq_s2_offsets_.resize(m + 1, 0);
  for (size_t a = 0; a < m; ++a) {
    result.arc_seq_s2_offsets_[a] =
        static_cast<uint32_t>(result.arc_seq_s2_ids_.size());
    result.arc_seq_s2_ids_.insert(result.arc_seq_s2_ids_.end(),
                                  per_arc_seq_s2[a].begin(),
                                  per_arc_seq_s2[a].end());
  }
  result.arc_seq_s2_offsets_[m] =
      static_cast<uint32_t>(result.arc_seq_s2_ids_.size());

  if (enforce_benchmark_assertions) {
    verify_canonical_counts(graph.name(), cut_model,
                            result.sim_scenarios_.size(),
                            result.seq_scenarios_.size());
  }

  return result;
}

}  // namespace ton
