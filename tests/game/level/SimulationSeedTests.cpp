#include <array>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "game/level/SimulationSeed.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(simulationSeed("") == 0x811c9dc5U);
static_assert(simulationSeed("G1") == 0x0fd076b5U);

TEST_CASE("native stage seeds use fixed unsigned 32-bit FNV-1a vectors",
          "[simulation-seed][determinism]") {
    struct Vector {
        std::string_view name;
        u32 seed;
    };
    constexpr std::array kVectors{
        Vector{"", 0x811c9dc5U},   Vector{"a", 0xe40c292cU},  Vector{"foobar", 0xbf9cf968U},
        Vector{"G1", 0x0fd076b5U}, Vector{"L1", 0x17e1cc6eU}, Vector{"B4", 0x08dd37a3U},
        Vector{"g1", 0x10202895U},
    };
    for (const auto& vector : kVectors) {
        CAPTURE(vector.name);
        CHECK(simulationSeed(vector.name) == vector.seed);
        // Owning strings and substrings must hash the same bytes, not addresses
        // or their enclosing allocation/terminator.
        const std::string surrounded = "[" + std::string{vector.name} + "]";
        CHECK(simulationSeed(std::string_view{surrounded}.substr(1, vector.name.size())) ==
              vector.seed);
    }
}

TEST_CASE("native stage seeds retain zero and high-bit bytes without signed-char extension",
          "[simulation-seed][determinism]") {
    constexpr std::string_view kZero{"G1\0", 3};
    constexpr std::string_view kHigh{"G\x80", 2};
    CHECK(simulationSeed(kZero) == 0x9a2adeefU);
    CHECK(simulationSeed(kHigh) == 0x9ecfc4d2U);
    CHECK(simulationSeed(kZero.substr(0, 2)) == 0x0fd076b5U);
    CHECK(simulationSeed(kZero) != simulationSeed(kZero.substr(0, 2)));
}
} // namespace
