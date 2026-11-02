#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/world/ThermalMaterial.h"

namespace {

using namespace gdl;

TEST_CASE("a thermal material names nothing until its caller does", "[heat][thermal-material]") {
    const ThermalMaterial none;
    CHECK(none.particles.empty());
    CHECK(none.surfaces.empty());
    for (const std::string_view name : {"P_TORCH", "TORCHB", "FIRE", ""}) {
        CAPTURE(name);
        CHECK_FALSE(none.particle(name));
        CHECK_FALSE(none.surface(name));
    }
}

TEST_CASE("a thermal material matches whole names, particles and surfaces apart",
          "[heat][thermal-material]") {
    static constexpr std::array<std::string_view, 2> kParticles{"SPARK", "EMBER"};
    static constexpr std::array<std::string_view, 1> kSurfaces{"HEARTH"};
    constexpr ThermalMaterial kThermal{kParticles, kSurfaces};
    CHECK(kThermal.particle("SPARK"));
    CHECK(kThermal.particle("EMBER"));
    CHECK(kThermal.surface("HEARTH"));
    CHECK_FALSE(kThermal.surface("SPARK")); // a flame particle is not a burning surface
    CHECK_FALSE(kThermal.particle("HEARTH"));
    for (const std::string_view name : {"SPARK2", "SPAR", "spark", "EMBER ", ""}) {
        CAPTURE(name);
        CHECK_FALSE(kThermal.particle(name));
    }
}

} // namespace
