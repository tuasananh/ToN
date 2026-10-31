#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace ton {

// Link spectral capacity: W = 32 orthogonal wavelengths mapped 1-to-1 onto uint32_t.
inline constexpr uint8_t NUM_WAVELENGTHS = 32;
using WavelengthMask = uint32_t;
inline constexpr WavelengthMask FULL_MASK = 0xFFFFFFFFu;
inline constexpr WavelengthMask EMPTY_MASK = 0x00000000u;
inline constexpr uint8_t INVALID_WAVELENGTH = 0xFFu;

// Compact strongly-meaningful integer identifiers for L1/L3 cache residency.
using NodeId = uint16_t;
using ArcId = uint16_t;
using SpanId = uint16_t;
using FailureUnitId = uint16_t;
using ScenarioId = uint32_t;
using RequestId = uint16_t;
using PathId = uint32_t;

inline constexpr NodeId INVALID_NODE = 0xFFFFu;
inline constexpr ArcId INVALID_ARC = 0xFFFFu;
inline constexpr SpanId INVALID_SPAN = 0xFFFFu;
inline constexpr FailureUnitId INVALID_FAILURE_UNIT = 0xFFFFu;
inline constexpr ScenarioId INVALID_SCENARIO = 0xFFFFFFFFu;
inline constexpr RequestId INVALID_REQUEST = 0xFFFFu;
inline constexpr PathId INVALID_PATH = 0xFFFFFFFFu;

// Canonical Earth radius (IUGG mean radius in km) shared across offline Python & C++23.
inline constexpr double EARTH_RADIUS_KM = 6371.0;

enum class DistanceMetric : uint8_t {
    GeographicKm = 0,
    HopCount = 1,
};

enum class CutModel : uint8_t {
    Span = 0,
    Directional = 1,
};

enum class FailureRegime : uint8_t {
    Simultaneous = 0,
    Sequential = 1,
};

[[nodiscard]] constexpr WavelengthMask wavelength_bit(uint8_t w) noexcept {
    return static_cast<WavelengthMask>(1u) << w;
}

[[nodiscard]] constexpr bool has_wavelength(WavelengthMask mask, uint8_t w) noexcept {
    return (mask & wavelength_bit(w)) != 0u;
}

[[nodiscard]] constexpr uint8_t count_wavelengths(WavelengthMask mask) noexcept {
    return static_cast<uint8_t>(std::popcount(mask));
}

// Lowest index available wavelength (lambda_0 -> lambda_{W-1}, ascending primary convention).
[[nodiscard]] constexpr uint8_t lowest_wavelength(WavelengthMask free_mask) noexcept {
    return free_mask == EMPTY_MASK ? INVALID_WAVELENGTH
                                   : static_cast<uint8_t>(std::countr_zero(free_mask));
}

// Highest index available wavelength (lambda_{W-1} -> lambda_0, descending backup convention).
[[nodiscard]] constexpr uint8_t highest_wavelength(WavelengthMask free_mask) noexcept {
    return free_mask == EMPTY_MASK ? INVALID_WAVELENGTH
                                   : static_cast<uint8_t>(31 - std::countl_zero(free_mask));
}

struct Request {
    RequestId id = INVALID_REQUEST;
    NodeId src = INVALID_NODE;
    NodeId dst = INVALID_NODE;

    [[nodiscard]] constexpr bool operator==(const Request&) const noexcept = default;
};

struct Lightpath {
    PathId path_id = INVALID_PATH;
    uint8_t wavelength = INVALID_WAVELENGTH;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return path_id != INVALID_PATH && wavelength < NUM_WAVELENGTHS;
    }

    [[nodiscard]] constexpr bool operator==(const Lightpath&) const noexcept = default;
};

// Fixed-capacity stack record holding up to 4 severed directed arcs (zero heap allocation).
struct CutArcs {
    std::array<ArcId, 4> arcs{INVALID_ARC, INVALID_ARC, INVALID_ARC, INVALID_ARC};
    uint8_t count = 0;

    constexpr void add_unique(ArcId arc) noexcept {
        for (uint8_t i = 0; i < count; ++i) {
            if (arcs[i] == arc) {
                return;
            }
        }
        arcs[count++] = arc;
        // Maintain canonical ascending ArcId order for O(1) topological key equality.
        for (uint8_t i = count - 1; i > 0 && arcs[i] < arcs[i - 1]; --i) {
            std::swap(arcs[i], arcs[i - 1]);
        }
    }

    [[nodiscard]] constexpr bool contains(ArcId arc) const noexcept {
        for (uint8_t i = 0; i < count; ++i) {
            if (arcs[i] == arc) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr std::span<const ArcId> span() const noexcept {
        return std::span<const ArcId>(arcs.data(), count);
    }

    [[nodiscard]] constexpr bool operator==(const CutArcs& other) const noexcept {
        if (count != other.count) {
            return false;
        }
        for (uint8_t i = 0; i < count; ++i) {
            if (arcs[i] != other.arcs[i]) {
                return false;
            }
        }
        return true;
    }
};

}  // namespace ton
