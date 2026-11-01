#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsScenery.h"
#include "game/combat/DamageTypes.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("Native shootable scenery has bounded physical health and independent collision",
          "[legends][native-scenery]") {
    const std::array definitions{formats::LegendsMapDestructible{7, {-4, 0, -4}, {4, 8, 4}},
                                 formats::LegendsMapDestructible{8, {10, 0, -4}, {18, 8, 4}}};
    LegendsScenery scenery{definitions, 1.5f};
    REQUIRE(scenery.targets().size() == 2);
    const auto target = scenery.targets().front();
    CHECK(target.base == Vec3{0, 3.5f, 0});
    CHECK(target.radius == 2);
    CHECK(target.height == 1);
    CHECK(scenery.health(0) == 45);
    CHECK_FALSE(scenery.strike(target.id, 100, damage::kMagic));
    CHECK_FALSE(scenery.strike(target.id, 100, damage::kGas));
    CHECK_FALSE(scenery.strike(target.id, std::numeric_limits<f32>::infinity(), 0));
    CHECK_FALSE(scenery.strike(-1, 100, 0));
    CHECK(scenery.whiteBlend(7) == 0);
    CHECK(scenery.strike(target.id, 20.9f, 0));
    CHECK(scenery.whiteBlend(7) == 1);
    CHECK(scenery.whiteBlend(8) == 0);
    scenery.update(1.0f / 60);
    CHECK(scenery.whiteBlend(7) == 1);
    scenery.update(1.0f / 60);
    CHECK(scenery.whiteBlend(7) == 0);
    CHECK(scenery.health(0) == 25);
    WorldCollision collision;
    scenery.syncCollision(collision);
    CHECK(collision.solid(7));
    CHECK(scenery.strike(target.id, 25, 0));
    CHECK_FALSE(scenery.visible(7));
    CHECK(scenery.whiteBlend(7) == 0);
    CHECK(scenery.visible(8));
    scenery.syncCollision(collision);
    CHECK_FALSE(collision.solid(7));
    CHECK(collision.solid(8));
    CHECK_FALSE(scenery.strike(target.id, 1, 0));
    REQUIRE(scenery.targets().size() == 1);
    CHECK(scenery.targets().front().id == target.id + 1);
}

TEST_CASE("Native scenery retains validated world faces for shared surface targeting",
          "[legends][native-scenery]") {
    const std::array definitions{formats::LegendsMapDestructible{7, {-4, 0, -4}, {4, 80, 4}}};
    const std::array faces{formats::LegendsMapCollisionTriangle{
        .vertices = {Vec3{-4, 0, -4}, Vec3{-4, 80, -4}, Vec3{4, 0, -4}},
        .normal = {0, 0, -1},
        .flags = 0x820,
        .owner = 7,
        .wall = true}};
    LegendsScenery scenery{definitions, 1, faces};
    const auto target = scenery.targets().front();
    CHECK(target.valid());
    REQUIRE(target.surface.size() == 1);
    CHECK(target.surface.front().vertices == faces.front().vertices);
    CHECK(target.surface.front().object == 7);
    CHECK(target.base.y == 0);
    CHECK(target.height == 80);
    auto invalid = faces;
    invalid[0].vertices[0].x = std::numeric_limits<f32>::infinity();
    CHECK_THROWS_AS(LegendsScenery(definitions, 1, invalid), std::invalid_argument);
    invalid = faces;
    invalid[0].normal = Vec3{0};
    CHECK_THROWS_AS(LegendsScenery(definitions, 1, invalid), std::invalid_argument);
}
