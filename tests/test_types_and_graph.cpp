#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "test_helpers.hpp"

TEST_CASE("Core bitmasks, wavelength operations, and types", "[core][types]") {
  TON_BOUNDS_CHECK(true, "sanity bounds check");
  TON_ASSERT(true, "sanity assertion");
  TON_ASSUME(true);

  static_assert(sizeof(ton::WavelengthMask) == 4);
  static_assert(ton::NUM_WAVELENGTHS == 32);
  static_assert(ton::count_wavelengths(ton::FULL_MASK) == 32);
  static_assert(ton::count_wavelengths(ton::EMPTY_MASK) == 0);

  REQUIRE(ton::lowest_wavelength(ton::FULL_MASK) == 0);
  REQUIRE(ton::highest_wavelength(ton::FULL_MASK) == 31);
  REQUIRE(ton::lowest_wavelength(ton::EMPTY_MASK) == ton::INVALID_WAVELENGTH);
  REQUIRE(ton::highest_wavelength(ton::EMPTY_MASK) == ton::INVALID_WAVELENGTH);

  const ton::WavelengthMask sample =
      ton::wavelength_bit(5) | ton::wavelength_bit(27);
  REQUIRE(ton::has_wavelength(sample, 5));
  REQUIRE(ton::has_wavelength(sample, 27));
  REQUIRE_FALSE(ton::has_wavelength(sample, 0));
  REQUIRE(ton::count_wavelengths(sample) == 2);
  REQUIRE(ton::lowest_wavelength(sample) == 5);
  REQUIRE(ton::highest_wavelength(sample) == 27);

  ton::CutArcs cut;
  cut.add_unique(19);
  cut.add_unique(3);
  cut.add_unique(19);
  cut.add_unique(11);
  REQUIRE(cut.count == 3);
  REQUIRE(cut.arcs[0] == 3);
  REQUIRE(cut.arcs[1] == 11);
  REQUIRE(cut.arcs[2] == 19);
  REQUIRE(cut.contains(11));
  REQUIRE_FALSE(cut.contains(7));

  ton::Lightpath lp;
  REQUIRE_FALSE(lp.valid());
  lp = ton::Lightpath{.path_id = 42, .wavelength = 31};
  REQUIRE(lp.valid());
}

TEST_CASE("Symmetric Digraph representation and SNDlib XML parser",
          "[core][graph]") {
  SECTION("Haversine precision & exact symmetry") {
    REQUIRE(ton::Graph::haversine_km(10.0, 50.0, 10.0, 50.0) == 0.0);
    const double eq_1deg = ton::Graph::haversine_km(0.0, 0.0, 1.0, 0.0);
    const double expected_eq_1deg =
        ton::EARTH_RADIUS_KM * (std::numbers::pi / 180.0);
    REQUIRE(std::abs(eq_1deg - expected_eq_1deg) < 1e-9);

    const double d_ab = ton::Graph::haversine_km(4.51, 52.20, 13.21, 52.31);
    const double d_ba = ton::Graph::haversine_km(13.21, 52.31, 4.51, 52.20);
    REQUIRE(d_ab == d_ba);
    REQUIRE(d_ab > 500.0);
    REQUIRE(d_ab < 700.0);
  }

  SECTION("Programmatic synthetic graph & deterministic CSR out-arc ordering") {
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
    const ton::Graph square("square4", nodes, spans,
                            ton::DistanceMetric::GeographicKm);
    REQUIRE(square.num_nodes() == 4);
    REQUIRE(square.num_spans() == 4);
    REQUIRE(square.num_arcs() == 8);

    const auto out0 = square.out_arcs(0);
    REQUIRE(out0.size() == 2);
    REQUIRE(square.arc(out0[0]).dst == 1);
    REQUIRE(square.arc(out0[1]).dst == 3);
    REQUIRE(square.arc_cost(out0[0], ton::DistanceMetric::GeographicKm) ==
            120.0);
    REQUIRE(square.arc_cost(out0[0], ton::DistanceMetric::HopCount) == 1.0);
    REQUIRE(square.find_arc(0, 1) == out0[0]);
    REQUIRE(square.find_arc(0, 2) == ton::INVALID_ARC);

    auto bad_spans = spans;
    bad_spans.push_back(
        {.name = "DUP", .u = 1, .v = 0, .override_length_km = 120.0});
    REQUIRE_THROWS_AS(ton::Graph("bad", nodes, bad_spans),
                      std::invalid_argument);
  }

  SECTION("Official SNDlib Nobel-EU & Germany50 topologies") {
    const auto nobel = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "nobel-eu.xml");
    REQUIRE(nobel.num_nodes() == 28);
    REQUIRE(nobel.num_spans() == 41);
    REQUIRE(nobel.num_arcs() == 82);
    REQUIRE(nobel.find_node("Amsterdam") == 0);
    REQUIRE(nobel.find_node("Zurich") == 27);

    const auto germany = ton::Graph::from_sndlib_xml(
        ton::test::repo_root() / "data" / "topologies" / "germany50.xml");
    REQUIRE(germany.num_nodes() == 50);
    REQUIRE(germany.num_spans() == 88);
    REQUIRE(germany.num_arcs() == 176);
    REQUIRE(germany.find_node("Aachen") == 0);
    REQUIRE(germany.find_node("Wuerzburg") == 49);

    for (const auto* g : {&nobel, &germany}) {
      for (ton::ArcId a = 0; a < g->num_arcs(); ++a) {
        const auto& arc = g->arc(a);
        REQUIRE(arc.id == a);
        REQUIRE(arc.src != arc.dst);
        REQUIRE(arc.length_km > 0.0);
        REQUIRE(std::isfinite(arc.length_km));
        const auto& rev = g->arc(arc.reverse_arc);
        REQUIRE(rev.reverse_arc == a);
        REQUIRE(rev.src == arc.dst);
        REQUIRE(rev.dst == arc.src);
        REQUIRE(rev.span_id == arc.span_id);
        REQUIRE(rev.length_km == arc.length_km);
        REQUIRE(g->find_arc(arc.src, arc.dst) == a);
      }
      for (ton::NodeId u = 0; u < g->num_nodes(); ++u) {
        const auto out = g->out_arcs(u);
        REQUIRE_FALSE(out.empty());
        for (size_t i = 1; i < out.size(); ++i) {
          REQUIRE(g->arc(out[i - 1]).dst < g->arc(out[i]).dst);
        }
      }
    }
  }
}

