#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/enemies/LegendsDeath.h"
#include "game/enemies/EnemyKinds.h"

namespace {
using namespace gdl;
using namespace gdl::game;
formats::LegendsN64Fixture statue() {
    formats::LegendsN64Fixture fixture;
    fixture.kind = formats::LegendsN64FixtureKind::Powerup;
    fixture.resolvedTemplateKey = 0x2100;
    fixture.sequence = "DETH";
    fixture.nativeIndex = 73;
    fixture.position = Vec3{3, 4, 5};
    return fixture;
}
} // namespace

TEST_CASE("Native Death statue requires contact or a real blow and releases only once",
          "[legends][native-death]") {
    auto death = LegendsDeath::decode(statue(), 1);
    REQUIRE(death);
    CHECK(death->nativeIndex() == 73);
    CHECK_FALSE(death->pending());
    CHECK(death->target(0).radius == Catch::Approx(51.f / 32));
    CHECK_FALSE(death->touch(Vec3{8, 4, 5}, 1, 3));
    CHECK_FALSE(death->touch(Vec3{3, 8, 5}, 1, 3));
    CHECK_FALSE(death->strike(0));
    CHECK_FALSE(death->strike(std::numeric_limits<f32>::quiet_NaN()));
    SECTION("contact") {
        REQUIRE(death->touch(Vec3{5, 4, 5}, 1, 3));
    }
    SECTION("blow compatibility") {
        REQUIRE(death->strike(1));
    }
    CHECK(death->pending());
    const auto spawn = death->spawn();
    CHECK(spawn.kind == kDeathKind);
    CHECK(spawn.tier == 1);
    CHECK(spawn.position == Vec3{3, 4, 5});
    CHECK(spawn.placed);
    CHECK_FALSE(spawn.asleep);
    death->release();
    CHECK_FALSE(death->pending());
    CHECK_FALSE(death->strike(100));
    CHECK_FALSE(death->touch(Vec3{3, 4, 5}, 1, 3));
    CHECK_FALSE(death->target(0).damageable);
}

TEST_CASE("Death admission rejects other pickups and party-gated placements",
          "[legends][native-death]") {
    auto fixture = statue();
    fixture.minimumPlayers = 2;
    CHECK_FALSE(LegendsDeath::decode(fixture, 1));
    CHECK(LegendsDeath::decode(fixture, 2));
    fixture.resolvedTemplateKey = 0x2000;
    CHECK_FALSE(LegendsDeath::decode(fixture, 2));
    fixture = statue();
    fixture.position.x = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(LegendsDeath::decode(fixture, 1));
}
