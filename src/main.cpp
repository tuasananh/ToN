#include <iostream>

#include "ton/core/types.hpp"

int main() {
    std::cout << "ToN 2026 Optical Survivability Engine (W="
              << static_cast<int>(ton::NUM_WAVELENGTHS) << ")\n";
    return 0;
}
