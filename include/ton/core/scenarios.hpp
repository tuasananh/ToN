#pragma once

#include <span>
#include <utility>
#include <vector>

#include "ton/core/graph.hpp"
#include "ton/core/types.hpp"

namespace ton {

struct SimScenario {
    ScenarioId id = INVALID_SCENARIO;
    FailureUnitId x1 = INVALID_FAILURE_UNIT;
    FailureUnitId x2 = INVALID_FAILURE_UNIT;
    CutArcs cut_arcs;
};

struct SeqScenario {
    ScenarioId id = INVALID_SCENARIO;
    FailureUnitId x1 = INVALID_FAILURE_UNIT;
    FailureUnitId x2 = INVALID_FAILURE_UNIT;
    // Unordered dual-cut topology ID in sim_scenarios() for O(1) Stage-2 path deduplication.
    ScenarioId sim_id = INVALID_SCENARIO;
    CutArcs stage1_cut;
    CutArcs stage2_cut;
    CutArcs total_cut;
};

// Tarjan's O(N + M) strong-connectivity test on G \ cut_arcs (zero heap allocation for N <= 128).
[[nodiscard]] bool is_strongly_connected(const Graph& graph, const CutArcs& cut_arcs);

class ScenarioSet {
public:
    ScenarioSet() = default;

    [[nodiscard]] static ScenarioSet enumerate(const Graph& graph, CutModel cut_model,
                                               bool enforce_benchmark_assertions = true);

    [[nodiscard]] CutModel cut_model() const noexcept { return cut_model_; }
    [[nodiscard]] FailureUnitId num_units() const noexcept { return num_units_; }

    [[nodiscard]] const CutArcs& unit_cut_arcs(FailureUnitId unit) const noexcept {
        return unit_cuts_[unit];
    }

    // Unique elementary failure unit x whose L_cut(x) contains directed arc `a`.
    [[nodiscard]] FailureUnitId arc_to_unit(ArcId a) const noexcept { return arc_to_unit_[a]; }

    [[nodiscard]] bool is_active_stage1_unit(FailureUnitId x1) const noexcept {
        return x1 < unit_is_active_s1_.size() && unit_is_active_s1_[x1] != 0;
    }

    [[nodiscard]] std::span<const SimScenario> sim_scenarios() const noexcept {
        return sim_scenarios_;
    }

    [[nodiscard]] std::span<const SeqScenario> seq_scenarios() const noexcept {
        return seq_scenarios_;
    }

    [[nodiscard]] const SimScenario& sim_scenario(ScenarioId id) const noexcept {
        return sim_scenarios_[id];
    }

    [[nodiscard]] const SeqScenario& seq_scenario(ScenarioId id) const noexcept {
        return seq_scenarios_[id];
    }

    [[nodiscard]] std::span<const FailureUnitId> active_stage1_units() const noexcept {
        return active_stage1_units_;
    }

    // Coupled sequential scenario group G_{x1} = {(x1, x2) in F_seq}, contiguous in memory.
    [[nodiscard]] std::span<const SeqScenario> seq_group(FailureUnitId x1) const noexcept {
        const auto [offset, len] = seq_group_ranges_[x1];
        return std::span<const SeqScenario>(seq_scenarios_.data() + offset, len);
    }

    // All simultaneous scenarios F in F_sim whose severed set L(F) contains directed arc `a`.
    [[nodiscard]] std::span<const ScenarioId> sim_scenarios_cutting_arc(ArcId a) const noexcept {
        const uint32_t begin = arc_sim_offsets_[a];
        const uint32_t end = arc_sim_offsets_[static_cast<size_t>(a) + 1];
        return std::span<const ScenarioId>(arc_sim_ids_.data() + begin, end - begin);
    }

    // All sequential scenarios F = (x1, x2) in F_seq whose second cut L_cut(x2) contains `a`.
    [[nodiscard]] std::span<const ScenarioId> seq_scenarios_stage2_cutting_arc(
        ArcId a) const noexcept {
        const uint32_t begin = arc_seq_s2_offsets_[a];
        const uint32_t end = arc_seq_s2_offsets_[static_cast<size_t>(a) + 1];
        return std::span<const ScenarioId>(arc_seq_s2_ids_.data() + begin, end - begin);
    }

private:
    CutModel cut_model_ = CutModel::Span;
    FailureUnitId num_units_ = 0;
    std::vector<CutArcs> unit_cuts_;
    std::vector<FailureUnitId> arc_to_unit_;
    std::vector<SimScenario> sim_scenarios_;
    std::vector<SeqScenario> seq_scenarios_;
    std::vector<FailureUnitId> active_stage1_units_;
    std::vector<uint8_t> unit_is_active_s1_;
    std::vector<std::pair<uint32_t, uint32_t>> seq_group_ranges_;

    std::vector<uint32_t> arc_sim_offsets_;
    std::vector<ScenarioId> arc_sim_ids_;
    std::vector<uint32_t> arc_seq_s2_offsets_;
    std::vector<ScenarioId> arc_seq_s2_ids_;
};

}  // namespace ton
