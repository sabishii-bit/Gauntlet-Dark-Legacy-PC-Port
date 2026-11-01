#include <cmath>
#include <limits>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsGates.h"

namespace {
using namespace gdl;
using namespace gdl::game;
formats::LegendsN64Gate definition() {
    formats::LegendsN64Gate gate;
    gate.templateKey = 0x803;
    gate.position = {10, 2, 8};
    gate.idleSequence = "DD1C";
    gate.openingSequence = "DD1O";
    gate.primary = {3, {3.5f, 5, .5f}, {0, 0}};
    gate.frames[0] = {3, {1.5f, 2.1f, .5f}, {3, 0}};
    gate.frames[1] = {3, {1.5f, 2.1f, .5f}, {-3, 0}};
    return gate;
}
} // namespace

TEST_CASE("A native locked gate debits exactly one key and immediately releases its primary body",
          "[legends][gates]") {
    LegendsGate gate{definition()};
    LegendsPortalVisitor visitor{0, {10, 2, 8.9f}, true, .75f, 3};
    s32 keys = 2;
    REQUIRE(gate.primaryObstacle().solid);
    REQUIRE(gate.touch(visitor, keys));
    CHECK(keys == 1);
    CHECK(gate.state() == LegendsGate::State::Opening);
    CHECK_FALSE(gate.primaryObstacle().solid);
    CHECK(gate.frameObstacles()[0].solid);
    CHECK(gate.frameObstacles()[1].solid);
    CHECK_FALSE(gate.touch(visitor, keys));
    CHECK(keys == 1);
    visitor.player = 1;
    s32 secondKeys = 4;
    CHECK_FALSE(gate.touch(visitor, secondKeys));
    CHECK(secondKeys == 4);
    REQUIRE(gate.finishOpening());
    CHECK(gate.state() == LegendsGate::State::Open);
    CHECK_FALSE(gate.finishOpening());
    CHECK_FALSE(gate.touch(visitor, secondKeys));
}

TEST_CASE("Native gates reject dead remote high and keyless contact", "[legends][gates]") {
    LegendsGate gate{definition()};
    LegendsPortalVisitor visitor{0, {10, 2, 8.9f}, true, .75f, 3};
    s32 keys = 1;
    SECTION("no key") {
        keys = 0;
    }
    SECTION("dead") {
        visitor.alive = false;
    }
    SECTION("far away") {
        visitor.position.x = 30;
    }
    SECTION("above reachable height") {
        visitor.position.y = 5.01f;
    }
    SECTION("below volume") {
        visitor.position.y = -2;
    }
    SECTION("non-finite") {
        visitor.position.x = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("invalid radius") {
        visitor.radius = -1;
    }
    SECTION("invalid player") {
        visitor.player = 4;
    }
    const s32 before = keys;
    CHECK_FALSE(gate.touch(visitor, keys));
    CHECK(keys == before);
    CHECK(gate.state() == LegendsGate::State::Closed);
    CHECK(gate.primaryObstacle().solid);
}

TEST_CASE("Gate box extents halve native dimensions and frame offsets rotate without scaling",
          "[legends][gates]") {
    auto native = definition();
    native.yaw = std::numbers::pi_v<f32> / 2;
    native.scale = 2;
    LegendsGate gate{native};
    CHECK(gate.primaryObstacle().halfAcross == 1.75f);
    CHECK(gate.primaryObstacle().halfAlong == .25f);
    CHECK(gate.primaryObstacle().height == 5);
    CHECK(gate.primaryObstacle().yaw == -native.yaw);
    const auto frames = gate.frameObstacles();
    CHECK(frames[0].centre.x == Catch::Approx(10));
    CHECK(frames[0].centre.z == Catch::Approx(11));
    CHECK(frames[0].halfAcross == .75f);
    CHECK(frames[0].height == 67.0f / 32);
    gate.placeAt({20, 5, 30});
    CHECK(gate.primaryObstacle().centre == Vec3{20, 5, 30});
    CHECK(gate.frameObstacles()[0].centre.z == Catch::Approx(33));
    s32 keys = 1;
    CHECK(gate.touch({0, {20.9f, 5, 30}, true, .75f, 3}, keys));
}

TEST_CASE("Initially triggered native gate opens once without spending a player's key",
          "[legends][gates]") {
    auto native = definition();
    native.placementFlags = 2;
    LegendsGate gate{native};
    CHECK(gate.state() == LegendsGate::State::Closed);
    CHECK(gate.activateInitially());
    CHECK_FALSE(gate.activateInitially());
    CHECK_FALSE(gate.primaryObstacle().solid);
    s32 keys = 2;
    CHECK_FALSE(gate.touch({0, native.position, true}, keys));
    CHECK(keys == 2);
}
