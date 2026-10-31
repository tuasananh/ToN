#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

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

}  // namespace

int main() {
    test_core_types_and_bitmasks();
    std::cout << "[PASS] All ToN C++23 unit tests passed.\n";
    return EXIT_SUCCESS;
}
