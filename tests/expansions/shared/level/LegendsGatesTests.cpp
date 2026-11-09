#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsGates.h"
#include "expansions/shared/level/LegendsGates.h"

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
    LegendsGate gate{legendsN64Gate(definition())};
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
    LegendsGate gate{legendsN64Gate(definition())};
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

TEST_CASE("Initially triggered native gate opens once without spending a player's key",
          "[legends][gates]") {
    auto native = definition();
    native.placementFlags = 2;
    LegendsGate gate{legendsN64Gate(native)};
    CHECK(gate.state() == LegendsGate::State::Closed);
    CHECK(gate.activateInitially());
    CHECK_FALSE(gate.activateInitially());
    CHECK_FALSE(gate.primaryObstacle().solid);
    s32 keys = 2;
    CHECK_FALSE(gate.touch({0, native.position, true}, keys));
    CHECK(keys == 2);
}
