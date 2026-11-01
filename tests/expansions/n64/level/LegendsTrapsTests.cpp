#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsTraps.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"

namespace {
using namespace gdl;
using namespace gdl::game;
formats::LegendsN64Trap definition() {
    formats::LegendsN64Trap result;
    result.templateKey = 0x1004;
    result.flags = 0x2081;
    result.position = {4, 2, 6};
    result.shape = 1;
    result.dimensions = {1, 3, 0};
    result.restFrames = 2;
    result.activeFrames = 4;
    result.damage = 5;
    return result;
}
void begin(LegendsTrap& trap) {
    for (s32 tick = 0; tick < 3; ++tick) {
        trap.advance(1.0f / 30, true, false, false);
    }
    REQUIRE(trap.phase() == LegendsTrap::Phase::Active);
    REQUIRE(trap.dangerous());
}
} // namespace

TEST_CASE("Native trap armor remains proportional at endgame attributes",
          "[legends][native-traps][damage]") {
    CHECK(legendsTrapDamage(4, 0) == 4);
    CHECK(legendsTrapDamage(4, 100) == 3);
    CHECK(legendsTrapDamage(4, 999) == 2);
    CHECK(legendsTrapDamage(5, 999) == 3);
    CHECK(legendsTrapDamage(1, 999) == 1);
    CHECK(legendsTrapDamage(4, -1) == 4);
    CHECK(legendsTrapDamage(4, std::numeric_limits<f32>::infinity()) == 0);
    CHECK(legendsTrapDamage(std::numeric_limits<f32>::quiet_NaN(), 0) == 0);
}

TEST_CASE("Native trap pulses hurt each player once and expire independently of presentation",
          "[legends][native-traps]") {
    LegendsTrap trap{definition(), 1.5f, 12};
    const LegendsPortalVisitor visitor{0, {4, 2, 6}, true, .75f, 3};
    CHECK_FALSE(trap.touch(visitor));
    begin(trap);
    const auto hit = trap.touch(visitor);
    REQUIRE(hit);
    CHECK(hit->damage == 7);
    CHECK_FALSE(hit->pierces);
    CHECK(hit->impact.flags == damage::kLow);
    CHECK_FALSE(trap.touch(visitor));
    auto peer = visitor;
    peer.player = 1;
    REQUIRE(trap.touch(peer));
    peer.player = 2;
    peer.alive = false;
    CHECK_FALSE(trap.touch(peer));
    peer.alive = true;
    peer.position.y = 10;
    CHECK_FALSE(trap.touch(peer));
    peer.position = {10, 2, 6};
    CHECK_FALSE(trap.touch(peer));
    peer.position = visitor.position;
    REQUIRE(trap.touch(peer));
    for (s32 i = 0; i < 4; ++i) {
        trap.advance(1.0f / 30, true, false, false);
    }
    CHECK_FALSE(trap.dangerous());
    CHECK(trap.phase() == LegendsTrap::Phase::Active);
    trap.advance(1.0f / 30, true, false, true);
    CHECK(trap.phase() == LegendsTrap::Phase::Idle);
}

TEST_CASE("Native trap idle admission visibility and Stop Time do not advance hidden attacks",
          "[legends][native-traps]") {
    LegendsTrap trap{definition(), 1, 12};
    trap.advance(3, false, false, false);
    CHECK(trap.phase() == LegendsTrap::Phase::Idle);
    trap.advance(3, true, true, false);
    CHECK(trap.phase() == LegendsTrap::Phase::Idle);
    begin(trap);
    trap.advance(1, true, true, false);
    CHECK_FALSE(trap.dangerous());
    CHECK_FALSE(trap.touch({0, {4, 2, 6}}));
    trap.advance(1.0f / 60, true, false, false);
    CHECK_FALSE(trap.dangerous()); // The separate attack lifetime elapsed while stopped.
}

TEST_CASE("Native trap start active and end figures use their own completion boundaries",
          "[legends][native-traps]") {
    auto native = definition();
    native.activeFrames = 0;
    LegendsTrap trap{native, 1, 12, 3, true};
    for (s32 i = 0; i < 3; ++i) {
        trap.advance(1.0f / 30, true, false, false);
    }
    REQUIRE(trap.phase() == LegendsTrap::Phase::Starting);
    REQUIRE(trap.dangerous());
    trap.advance(1.0f / 30, true, false, true);
    REQUIRE(trap.phase() == LegendsTrap::Phase::Active);
    for (s32 i = 0; i < 12; ++i) {
        trap.advance(1.0f / 30, true, false, true);
    }
    REQUIRE(trap.phase() == LegendsTrap::Phase::Active);
    trap.advance(1.0f / 30, true, false, true);
    REQUIRE(trap.phase() == LegendsTrap::Phase::Ending);
    trap.advance(1.0f / 30, true, false, false);
    CHECK(trap.phase() == LegendsTrap::Phase::Ending);
    trap.advance(1.0f / 30, true, false, true);
    CHECK(trap.phase() == LegendsTrap::Phase::Idle);
}

TEST_CASE("Native trap collision translates full box dimensions and semantic impact flags",
          "[legends][native-traps]") {
    auto native = definition();
    native.templateKey = 0x1005;
    native.flags = 0x2041;
    native.shape = 3;
    native.dimensions = {4, 2, .5f};
    native.yaw = 1.57079632679f;
    LegendsTrap trap{native, 1, 12};
    const auto body = trap.contact();
    CHECK_FALSE(body.solid);
    CHECK(body.halfAcross == 2);
    CHECK(body.halfAlong == .25f);
    begin(trap);
    CHECK_FALSE(trap.touch({0, {5.1f, 2, 6}, true, .5f, 3}));
    const auto hit = trap.touch({0, {4, 2, 7.9f}, true, .5f, 3});
    REQUIRE(hit);
    CHECK(hit->impact.flags == (damage::kKnockDown | damage::kLow));
    trap.placeAt({14, 5, 16});
    CHECK(trap.contact().centre == Vec3{14, 5, 16});
    CHECK_THROWS(trap.placeAt({std::numeric_limits<f32>::infinity(), 0, 0}));
}

TEST_CASE("Native trap clocks agree at30 and60Hz", "[legends][native-traps]") {
    LegendsTrap slow{definition(), 1, 12};
    LegendsTrap fast{definition(), 1, 12};
    for (s32 tick = 0; tick < 600; ++tick) {
        slow.advance(1.0f / 30, true, false, true);
        fast.advance(1.0f / 60, true, false, true);
        fast.advance(1.0f / 60, true, false, true);
        CHECK(slow.phase() == fast.phase());
        CHECK(slow.generation() == fast.generation());
        CHECK(slow.dangerous() == fast.dangerous());
    }
}

TEST_CASE("Native tentacle damage follows HITNODE and never substitutes the root placement",
          "[legends][native-traps]") {
    auto native = definition();
    native.templateKey = 0x2c07;
    native.flags = 0x200a; // Type10 supplies knockdown even without the ordinary trap flag.
    native.shape = 5;
    native.dimensions = {1, 0, 0};
    native.contactNode = "HITNODE";
    native.damage = 3;
    LegendsTrap trap{native, 7, 101};
    for (s32 tick = 0; tick < 3; ++tick) {
        trap.advance(1.0f / 30, true, false, false);
    }
    REQUIRE(trap.phase() == LegendsTrap::Phase::Active);
    CHECK_FALSE(trap.dangerous());
    CHECK_FALSE(trap.touch({0, native.position}));
    trap.nodeContact(Vec3{10.2f, 7.1f, -5.3f});
    REQUIRE(trap.dangerous());
    const auto body = trap.contact();
    CHECK(body.centre == Vec3{10.1875f, 4.09375f, -5.28125f});
    CHECK(body.height == 6);
    CHECK(body.cylinderRadius == 1);
    CHECK_FALSE(trap.touch({0, native.position}));
    CHECK_FALSE(trap.touch({0, {10.2f, 10.2f, -5.3f}, true, .5f, 3}));
    const auto hit = trap.touch({0, {10.2f, 5, -5.3f}, true, .5f, 3});
    REQUIRE(hit);
    CHECK(hit->damage == 3); // Native limb contacts intentionally bypass stage scaling.
    CHECK(hit->impact.flags == (damage::kLow | damage::kKnockDown));
    trap.nodeContact(Vec3{-20, 2, 8});
    CHECK_FALSE(trap.touch({1, {10.2f, 5, -5.3f}, true, .5f, 3}));
    REQUIRE(trap.touch({1, {-20, 0, 8}, true, .5f, 3}));
    trap.nodeContact(std::nullopt);
    CHECK_FALSE(trap.dangerous());
    CHECK_THROWS(trap.nodeContact(Vec3{std::numeric_limits<f32>::infinity(), 0, 0}));
}
