#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"
#include "engine/world/WorldLighting.h"

namespace {

using namespace gdl;
using Catch::Approx;

TEST_CASE("a level's light shades surfaces by ambient plus the light they face",
          "[world][lighting]") {
    const WorldLighting lighting =
        WorldLighting::forLevel(0.8f, Vec3{-1.0f, -6.0f, 2.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);
    REQUIRE(lighting.ambient == Vec3{0.8f, 0.8f, 0.8f});
    REQUIRE(lighting.lightColor == Vec3{1.0f, 1.0f, 1.0f});
    // The direction points back towards the light, which shines down the tower's hall.
    const Vec3 expected = glm::normalize(Vec3{1.0f, 6.0f, -2.0f});
    REQUIRE(glm::all(glm::epsilonEqual(lighting.direction, expected, 1e-5f)));

    // A floor facing the light saturates; a ceiling facing away keeps the ambient alone.
    REQUIRE(lighting.shade(Vec3{0.0f, 1.0f, 0.0f}) == Color::rgba(255, 255, 255));
    REQUIRE(lighting.shade(Vec3{0.0f, -1.0f, 0.0f}) == Color::rgba(204, 204, 204));
    // A wall catches a little: 0.8 + 1 / sqrt(41).
    const Color wall = lighting.shade(Vec3{1.0f, 0.0f, 0.0f});
    REQUIRE(static_cast<s32>(wall.r) == Approx(244).margin(1.0));
    REQUIRE(wall.a == 255);

    // Colour and intensity scale the light, not the ambient.
    const WorldLighting tinted =
        WorldLighting::forLevel(0.2f, Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 0.5f, 0.0f}, 0.5f);
    REQUIRE(tinted.shade(Vec3{0.0f, 1.0f, 0.0f}) == Color::rgba(179, 115, 51));
    // A zero direction falls back to light from above.
    REQUIRE(WorldLighting::forLevel(0.5f, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f)
                .direction == Vec3{0.0f, 1.0f, 0.0f});
}

TEST_CASE("the default light is the menu layer's and a level record converts directly",
          "[world][lighting]") {
    const WorldLighting menu;
    REQUIRE(menu.shade(glm::normalize(menu.direction)) == Color::rgba(255, 255, 255));
    REQUIRE(menu.shade(-glm::normalize(menu.direction)) == Color::rgba(140, 140, 140));

    LevelInfo level;
    level.ambient = 0.8f;
    level.lightDirection = Vec3{-1.0f, -6.0f, 2.0f};
    level.lightColor = Vec3{1.0f, 1.0f, 1.0f};
    level.lightIntensity = 1.0f;
    const WorldLighting fromLevel = WorldLighting::forLevel(level);
    REQUIRE(fromLevel.ambient.x == Approx(0.8f));
    REQUIRE(fromLevel.shade(Vec3{0.0f, -1.0f, 0.0f}) == Color::rgba(204, 204, 204));
}

} // namespace
