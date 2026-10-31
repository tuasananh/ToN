#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ton/core/graph.hpp"
#include "ton/core/scenarios.hpp"
#include "ton/core/types.hpp"

namespace {

[[noreturn]] void fail_check(std::string_view expr, std::string_view msg,
                             const std::source_location& loc = std::source_location::current()) {
    std::cerr << "[FAIL] " << loc.file_name() << ":" << loc.line() << " in " << loc.function_name()
              << "\n  Expression: " << expr;
    if (!msg.empty()) {
        std::cerr << "\n  Detail: " << msg;
    }
    std::cerr << "\n";
    std::exit(EXIT_FAILURE);
}

#define TON_CHECK(cond)            \
    do {                           \
        if (!(cond)) {             \
            fail_check(#cond, ""); \
        }                          \
    } while (false)

#define TON_CHECK_MSG(cond, msg)      \
    do {                              \
        if (!(cond)) {                \
            fail_check(#cond, (msg)); \
        }                             \
    } while (false)

void test_core_types_and_bitmasks() {
    static_assert(sizeof(ton::WavelengthMask) == 4);
    static_assert(ton::NUM_WAVELENGTHS == 32);
    static_assert(ton::count_wavelengths(ton::FULL_MASK) == 32);
    static_assert(ton::count_wavelengths(ton::EMPTY_MASK) == 0);

    TON_CHECK(ton::lowest_wavelength(ton::FULL_MASK) == 0);
    TON_CHECK(ton::highest_wavelength(ton::FULL_MASK) == 31);
    TON_CHECK(ton::lowest_wavelength(ton::EMPTY_MASK) == ton::INVALID_WAVELENGTH);
    TON_CHECK(ton::highest_wavelength(ton::EMPTY_MASK) == ton::INVALID_WAVELENGTH);

    const ton::WavelengthMask sample = ton::wavelength_bit(5) | ton::wavelength_bit(27);
    TON_CHECK(ton::has_wavelength(sample, 5));
    TON_CHECK(ton::has_wavelength(sample, 27));
    TON_CHECK(!ton::has_wavelength(sample, 0));
    TON_CHECK(ton::count_wavelengths(sample) == 2);
    TON_CHECK(ton::lowest_wavelength(sample) == 5);
    TON_CHECK(ton::highest_wavelength(sample) == 27);

    ton::CutArcs cut;
    cut.add_unique(19);
    cut.add_unique(3);
    cut.add_unique(19);
    cut.add_unique(11);
    TON_CHECK(cut.count == 3);
    TON_CHECK(cut.arcs[0] == 3 && cut.arcs[1] == 11 && cut.arcs[2] == 19);
    TON_CHECK(cut.contains(11));
    TON_CHECK(!cut.contains(7));

    ton::Lightpath lp;
    TON_CHECK(!lp.valid());
    lp = ton::Lightpath{.path_id = 42, .wavelength = 31};
    TON_CHECK(lp.valid());
}

std::filesystem::path repo_root() {
    return std::filesystem::path(TON_REPO_ROOT);
}

void test_symmetric_digraph_and_sndlib_xml() {
    // 1. Haversine precision & exact symmetry checks.
    TON_CHECK(ton::Graph::haversine_km(10.0, 50.0, 10.0, 50.0) == 0.0);
    const double eq_1deg = ton::Graph::haversine_km(0.0, 0.0, 1.0, 0.0);
    const double expected_eq_1deg = ton::EARTH_RADIUS_KM * (std::numbers::pi / 180.0);
    TON_CHECK(std::abs(eq_1deg - expected_eq_1deg) < 1e-9);

    const double d_ab = ton::Graph::haversine_km(4.51, 52.20, 13.21, 52.31);
    const double d_ba = ton::Graph::haversine_km(13.21, 52.31, 4.51, 52.20);
    TON_CHECK(d_ab == d_ba);
    TON_CHECK(d_ab > 500.0 && d_ab < 700.0);

    // 2. Programmatic synthetic graph & deterministic CSR out-arc ordering.
    std::vector<ton::NodeInfo> nodes = {
        {.id = 0, .name = "N0", .lon_deg = 0.0, .lat_deg = 0.0},
        {.id = 1, .name = "N1", .lon_deg = 1.0, .lat_deg = 0.0},
        {.id = 2, .name = "N2", .lon_deg = 1.0, .lat_deg = 1.0},
        {.id = 3, .name = "N3", .lon_deg = 0.0, .lat_deg = 1.0},
    };
    std::vector<ton::SpanInput> spans = {
        {.name = "S_0_3", .u = 0, .v = 3, .override_length_km = 100.0},
        {.name = "S_0_1", .u = 0, .v = 1, .override_length_km = 120.0},
        {.name = "S_1_2", .u = 1, .v = 2, .override_length_km = 100.0},
        {.name = "S_2_3", .u = 2, .v = 3, .override_length_km = 120.0},
    };
    const ton::Graph square("square4", nodes, spans, ton::DistanceMetric::GeographicKm);
    TON_CHECK(square.num_nodes() == 4);
    TON_CHECK(square.num_spans() == 4);
    TON_CHECK(square.num_arcs() == 8);

    // Outgoing arcs from N0 must be sorted by ascending target NodeId: (0->1) before (0->3).
    const auto out0 = square.out_arcs(0);
    TON_CHECK(out0.size() == 2);
    TON_CHECK(square.arc(out0[0]).dst == 1);
    TON_CHECK(square.arc(out0[1]).dst == 3);
    TON_CHECK(square.arc_cost(out0[0], ton::DistanceMetric::GeographicKm) == 120.0);
    TON_CHECK(square.arc_cost(out0[0], ton::DistanceMetric::HopCount) == 1.0);
    TON_CHECK(square.find_arc(0, 1) == out0[0]);
    TON_CHECK(square.find_arc(0, 2) == ton::INVALID_ARC);

    // Reject duplicate span.
    bool threw_dup = false;
    try {
        auto bad_spans = spans;
        bad_spans.push_back({.name = "DUP", .u = 1, .v = 0, .override_length_km = 120.0});
        (void)ton::Graph("bad", nodes, bad_spans);
    } catch (const std::invalid_argument&) {
        threw_dup = true;
    }
    TON_CHECK(threw_dup);

    // 3. Official SNDlib Nobel-EU (28N, 41E, 82L) & Germany50 (50N, 88E, 176L).
    const auto nobel =
        ton::Graph::from_sndlib_xml(repo_root() / "data" / "topologies" / "nobel-eu.xml");
    TON_CHECK(nobel.num_nodes() == 28);
    TON_CHECK(nobel.num_spans() == 41);
    TON_CHECK(nobel.num_arcs() == 82);
    TON_CHECK(nobel.find_node("Amsterdam") == 0);
    TON_CHECK(nobel.find_node("Zurich") == 27);

    const auto germany =
        ton::Graph::from_sndlib_xml(repo_root() / "data" / "topologies" / "germany50.xml");
    TON_CHECK(germany.num_nodes() == 50);
    TON_CHECK(germany.num_spans() == 88);
    TON_CHECK(germany.num_arcs() == 176);
    TON_CHECK(germany.find_node("Aachen") == 0);
    TON_CHECK(germany.find_node("Wuerzburg") == 49);

    for (const auto* g : {&nobel, &germany}) {
        for (ton::ArcId a = 0; a < g->num_arcs(); ++a) {
            const auto& arc = g->arc(a);
            TON_CHECK(arc.id == a);
            TON_CHECK(arc.src != arc.dst);
            TON_CHECK(arc.length_km > 0.0 && std::isfinite(arc.length_km));
            const auto& rev = g->arc(arc.reverse_arc);
            TON_CHECK(rev.reverse_arc == a);
            TON_CHECK(rev.src == arc.dst && rev.dst == arc.src);
            TON_CHECK(rev.span_id == arc.span_id);
            TON_CHECK(rev.length_km == arc.length_km);
            TON_CHECK(g->find_arc(arc.src, arc.dst) == a);
        }
        for (ton::NodeId u = 0; u < g->num_nodes(); ++u) {
            const auto out = g->out_arcs(u);
            TON_CHECK(!out.empty());
            for (size_t i = 1; i < out.size(); ++i) {
                TON_CHECK(g->arc(out[i - 1]).dst < g->arc(out[i]).dst);
            }
        }
    }
}

void test_dual_cut_scenarios_and_tarjan_scc() {
    // 1. Synthetic 4-cycle ("square"): cutting any 2 spans disconnects the 4-cycle (0 Sim Span),
    // while cutting 2 counter-clockwise arcs leaves the clockwise 4-cycle strongly connected!
    std::vector<ton::NodeInfo> nodes = {
        {.id = 0, .name = "N0", .lon_deg = 0.0, .lat_deg = 0.0},
        {.id = 1, .name = "N1", .lon_deg = 1.0, .lat_deg = 0.0},
        {.id = 2, .name = "N2", .lon_deg = 1.0, .lat_deg = 1.0},
        {.id = 3, .name = "N3", .lon_deg = 0.0, .lat_deg = 1.0},
    };
    std::vector<ton::SpanInput> spans = {
        {.name = "S01", .u = 0, .v = 1, .override_length_km = 10.0},
        {.name = "S12", .u = 1, .v = 2, .override_length_km = 10.0},
        {.name = "S23", .u = 2, .v = 3, .override_length_km = 10.0},
        {.name = "S30", .u = 3, .v = 0, .override_length_km = 10.0},
    };
    const ton::Graph square("square4", nodes, spans);
    TON_CHECK(ton::is_strongly_connected(square, ton::CutArcs{}));

    const auto square_span = ton::ScenarioSet::enumerate(square, ton::CutModel::Span);
    TON_CHECK(square_span.sim_scenarios().empty());
    TON_CHECK(square_span.seq_scenarios().empty());

    // In a bidirected 4-cycle (8 arcs, C(8,2)=28 pairs): 12 same-orientation pairs + 4 single-span
    // pairs leave the surviving digraph strongly connected -> 16 Sim, 32 Seq.
    const auto square_dir = ton::ScenarioSet::enumerate(square, ton::CutModel::Directional);
    TON_CHECK(square_dir.sim_scenarios().size() == 16);
    TON_CHECK(square_dir.seq_scenarios().size() == 32);

    // 2. Hard-assert exact canonical benchmark scenario counts (PROBLEM.md Section 3.2):
    //    Nobel-EU:  804 / 1,608 (Span),   3,289 / 6,578 (Directional)
    //    Germany50: 3,817 / 7,634 (Span), 15,378 / 30,756 (Directional)
    const auto nobel =
        ton::Graph::from_sndlib_xml(repo_root() / "data" / "topologies" / "nobel-eu.xml");
    const auto germany =
        ton::Graph::from_sndlib_xml(repo_root() / "data" / "topologies" / "germany50.xml");

    const auto nobel_span = ton::ScenarioSet::enumerate(nobel, ton::CutModel::Span);
    TON_CHECK(nobel_span.sim_scenarios().size() == 804);
    TON_CHECK(nobel_span.seq_scenarios().size() == 1608);

    const auto nobel_dir = ton::ScenarioSet::enumerate(nobel, ton::CutModel::Directional);
    TON_CHECK(nobel_dir.sim_scenarios().size() == 3289);
    TON_CHECK(nobel_dir.seq_scenarios().size() == 6578);

    const auto germany_span = ton::ScenarioSet::enumerate(germany, ton::CutModel::Span);
    TON_CHECK(germany_span.sim_scenarios().size() == 3817);
    TON_CHECK(germany_span.seq_scenarios().size() == 7634);

    const auto germany_dir = ton::ScenarioSet::enumerate(germany, ton::CutModel::Directional);
    TON_CHECK(germany_dir.sim_scenarios().size() == 15378);
    TON_CHECK(germany_dir.seq_scenarios().size() == 30756);

    // 3. Structural invariants across all 4 benchmark ScenarioSets:
    //    - |F_seq| == 2 * |F_sim|
    //    - Every active Stage-1 unit x1 leaves G \ L_cut(x1) strongly connected
    //    - Every coupled group G_{x1} is contiguous and maps (x1, x2) & (x2, x1) to identical
    //    sim_id
    for (const auto& [g_ptr, sc_ptr] : {
             std::pair{&nobel, &nobel_span},
             std::pair{&nobel, &nobel_dir},
             std::pair{&germany, &germany_span},
             std::pair{&germany, &germany_dir},
         }) {
        const auto& g = *g_ptr;
        const auto& sc_set = *sc_ptr;
        TON_CHECK(sc_set.seq_scenarios().size() == 2 * sc_set.sim_scenarios().size());

        size_t total_grouped = 0;
        for (const ton::FailureUnitId x1 : sc_set.active_stage1_units()) {
            TON_CHECK(sc_set.is_active_stage1_unit(x1));
            TON_CHECK(ton::is_strongly_connected(g, sc_set.unit_cut_arcs(x1)));
            const auto grp = sc_set.seq_group(x1);
            TON_CHECK(!grp.empty());
            total_grouped += grp.size();
            for (const auto& seq_sc : grp) {
                TON_CHECK(seq_sc.x1 == x1 && seq_sc.x2 != x1);
                const auto& sim_sc = sc_set.sim_scenario(seq_sc.sim_id);
                TON_CHECK(sim_sc.cut_arcs == seq_sc.total_cut);
                TON_CHECK((sim_sc.x1 == seq_sc.x1 && sim_sc.x2 == seq_sc.x2) ||
                          (sim_sc.x1 == seq_sc.x2 && sim_sc.x2 == seq_sc.x1));
            }
        }
        TON_CHECK(total_grouped == sc_set.seq_scenarios().size());
    }
}

}  // namespace

int main() {
    test_core_types_and_bitmasks();
    test_symmetric_digraph_and_sndlib_xml();
    test_dual_cut_scenarios_and_tarjan_scc();
    std::cout << "[PASS] All ToN C++23 unit tests passed.\n";
    return EXIT_SUCCESS;
}
