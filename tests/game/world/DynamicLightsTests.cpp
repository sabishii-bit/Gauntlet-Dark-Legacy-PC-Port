#include <catch2/catch_test_macros.hpp>

#include "engine/math/Math.h"

#include "game/world/DynamicLights.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("the lights take the original's colours", "[game][world][lights]") {
    // Costume colours (yellow, blue, red, green) light their own colour.
    CHECK(DynamicLights::ofCostume(0) == Vec3{2, 2, 0});
    CHECK(DynamicLights::ofCostume(1) == Vec3{0, 0, 2});
    CHECK(DynamicLights::ofCostume(2) == Vec3{2, 0, 0});
    CHECK(DynamicLights::ofCostume(3) == Vec3{0, 2, 0});
    // Damage types: normal white, fire red, electric white, light yellow, acid green.
    CHECK(DynamicLights::ofDamage(0) == Vec3{1, 1, 1});
    CHECK(DynamicLights::ofDamage(1) == Vec3{2, 0, 0});
    CHECK(DynamicLights::ofDamage(2) == Vec3{1, 1, 1});
    CHECK(DynamicLights::ofDamage(3) == Vec3{2, 2, 0});
    CHECK(DynamicLights::ofDamage(4) == Vec3{0, 2, 0});
    // A potion's is its element's; the first two kinds both burn.
    CHECK(DynamicLights::ofPotion(0) == Vec3{2, 0, 0});
    CHECK(DynamicLights::ofPotion(4) == Vec3{0, 2, 0});
    // The classes' own, warrior red to archer green, then again.
    CHECK(DynamicLights::ofClass(0) == Vec3{2, 0, 0});
    CHECK(DynamicLights::ofClass(2) == Vec3{0, 2, 2});
    CHECK(DynamicLights::ofClass(7) == Vec3{0, 2, 0});
    CHECK(DynamicLights::lantern(1) == Vec3{1.5f, 1.5f, 2.0f});
}

} // namespace
