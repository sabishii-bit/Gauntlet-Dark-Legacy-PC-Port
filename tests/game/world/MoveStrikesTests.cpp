#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/world/MoveStrikes.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("a burst harms what is about it once, its delay after it starts",
          "[game][world][strikes]") {
    MoveStrike burst;
    burst.type = MoveStrike::kBursts;
    burst.radius = 12.0f;
    burst.delay = 0.5f;
    burst.amount = 50.0f;
    burst.offset = Vec3{1.0f, 0.0f, 2.0f};
    MoveStrikes strikes;
    const Vec3 facing{0.0f, 0.0f, -1.0f};
    const u32 id = strikes.start(burst, 2, Vec3{10.0f, 0.0f, 10.0f}, facing, 8.0f);
    REQUIRE(id != 0);
    REQUIRE(strikes.count() == 1);
    // Its offset is in the body's space: two ahead, one to its side.
    REQUIRE(strikes.strike(0).position.z == Approx(8.0f));
    REQUIRE(strikes.strike(0).position.x == Approx(9.0f));
    REQUIRE(strikes.update(0.25f, nullptr).empty());
    const std::vector<StrikeHit> hits = strikes.update(0.3f, nullptr);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].strike == id);
    REQUIRE(hits[0].owner == 2);
    REQUIRE(hits[0].damage == 50.0f);
    REQUIRE(hits[0].radius == 12.0f);
    REQUIRE(strikes.count() == 0);
    REQUIRE(strikes.find(id) == nullptr);
    REQUIRE(strikes.update(1.0f, nullptr).empty());
    // A negative amount is so many times the character's own harm.
    burst.amount = -1.5f;
    REQUIRE(MoveStrikes::damageOf(burst, 8.0f) == 12.0f);
}

TEST_CASE("a hit reaches what stands within it, and within its arc when it has one",
          "[game][world][strikes]") {
    StrikeHit hit;
    hit.centre = Vec3{0.0f, 0.0f, 0.0f};
    hit.radius = 5.0f;
    hit.damage = 50.0f;
    hit.facing = Vec3{0.0f, 0.0f, 1.0f};
    REQUIRE(hit.reaches(Vec3{0.0f, 0.0f, 5.5f}, 1.0f, 3.0f));
    REQUIRE_FALSE(hit.reaches(Vec3{0.0f, 0.0f, 6.5f}, 1.0f, 3.0f));
    REQUIRE(hit.reaches(Vec3{0.0f, 0.0f, -4.0f}, 1.0f, 3.0f)); // all round
    REQUIRE_FALSE(hit.reaches(Vec3{0.0f, 9.0f, 1.0f}, 1.0f, 3.0f));
    REQUIRE(hit.reaches(Vec3{0.0f, -7.0f, 1.0f}, 1.0f, 3.0f)); // its top comes up into it
    hit.arc = 0.5f;                                            // sixty degrees either side of ahead
    REQUIRE(hit.reaches(Vec3{1.0f, 0.0f, 4.0f}, 1.0f, 3.0f));
    REQUIRE_FALSE(hit.reaches(Vec3{4.0f, 0.0f, 1.0f}, 1.0f, 3.0f));
    REQUIRE_FALSE(hit.reaches(Vec3{0.0f, 0.0f, -4.0f}, 1.0f, 3.0f));
    hit.damage = 0;
    CHECK_FALSE(hit.reaches(Vec3{0}, 1, 3));
    hit.damage = 10;
    hit.radius = 0;
    CHECK_FALSE(hit.reaches(Vec3{0}, 1, 3)); // a presentation-only burst has no damage volume
}

TEST_CASE("what a move sends flying goes on ahead, harming as it goes, until its time is up",
          "[game][world][strikes]") {
    MoveStrike wave;
    wave.type = MoveStrike::kFlies;
    wave.hitRadius = 10.0f;
    wave.maxTime = 2.0f;
    wave.speed = 30.0f;
    wave.amount = 70.0f;
    wave.offset = Vec3{0.0f, 1.0f, 5.0f};
    MoveStrikes strikes;
    const u32 id = strikes.start(wave, 0, Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 8.0f);
    REQUIRE(strikes.find(id) != nullptr);
    REQUIRE(strikes.find(id)->flies);
    REQUIRE(strikes.find(id)->position == Vec3{5.0f, 1.0f, 0.0f});
    std::vector<StrikeHit> hits = strikes.update(0.5f, nullptr);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].centre.x == Approx(20.0f));
    REQUIRE(hits[0].radius == 10.0f);
    REQUIRE(hits[0].arc == -1.0f);
    hits = strikes.update(1.0f, nullptr);
    REQUIRE(hits[0].strike == id);
    REQUIRE(hits[0].centre.x == Approx(50.0f));
    strikes.update(0.6f, nullptr);
    REQUIRE(strikes.count() == 0);
    strikes.start(wave, 0, Vec3{0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 8.0f);
    strikes.clear();
    REQUIRE(strikes.count() == 0);
}

TEST_CASE("turbo hit volumes sweep short targets and stop at their authored lifetime",
          "[game][world][strikes]") {
    MoveStrike row;
    row.type = MoveStrike::kFlies;
    row.hitRadius = 0.5f;
    row.speed = 30;
    row.maxTime = 0.5f;
    row.amount = 70;
    MoveStrikes strikes;
    strikes.start(row, 0, Vec3{0, 2, 0}, Vec3{0, 0, 1}, 10);
    REQUIRE(strikes.update(0, nullptr).empty());
    const auto hits = strikes.update(1, nullptr);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].centre.z == Approx(15));
    CHECK(hits[0].reaches({0, 0, 5}, 0.5f, 3));
    CHECK_FALSE(hits[0].reaches({0, 10, 5}, 0.5f, 3));
    CHECK_FALSE(hits[0].reaches({3, 0, 5}, 0.5f, 3));
    CHECK_FALSE(hits[0].reaches({0, 0, 20}, 0.5f, 3));
    CHECK(strikes.count() == 0);
}

TEST_CASE("turbo waves obey yaw and explicit world-collision policy", "[game][world][strikes]") {
    WorldCollision world;
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-20, -20, 5}, Vec3{0, 20, 5}, Vec3{20, -20, 5}};
    world.build({wall});
    MoveStrike row;
    row.type = MoveStrike::kFlies;
    row.hitRadius = 0.1f;
    row.speed = 30;
    row.maxTime = 2;
    row.amount = 70;
    SECTION("ordinary collision cannot skip a wall on a long update") {
        MoveStrikes strikes;
        strikes.start(row, 0, {0, 2, 0}, {0, 0, 1}, 10);
        const auto hits = strikes.update(0.5f, &world);
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].centre.z < 5);
        CHECK_FALSE(hits[0].reaches({0, 0, 10}, 1, 4));
        CHECK(strikes.count() == 0);
    }
    SECTION("authored 0x40 turbo wave passes world geometry") {
        row.flags = 0x40;
        MoveStrikes strikes;
        strikes.start(row, 0, {0, 2, 0}, {0, 0, 1}, 10);
        const auto hits = strikes.update(0.5f, &world);
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].centre.z == Approx(15));
        CHECK(hits[0].reaches({0, 0, 10}, 1, 4));
        CHECK(strikes.count() == 1);
    }
    SECTION("authored yaw changes travel without rotating the launch offset") {
        row.angle = 1.57079632679f;
        row.offset = {0, 0, 2};
        MoveStrikes strikes;
        strikes.start(row, 0, {0, 2, 0}, {0, 0, 1}, 10);
        const auto hits = strikes.update(0.5f, nullptr);
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].centre.x == Approx(15));
        CHECK(hits[0].centre.z == Approx(2));
    }
}

} // namespace
