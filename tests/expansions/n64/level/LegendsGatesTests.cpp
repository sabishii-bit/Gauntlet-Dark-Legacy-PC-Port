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

TEST_CASE("Gate box extents halve native dimensions and frame offsets rotate without scaling",
          "[legends][gates]") {
    auto native = definition();
    native.yaw = std::numbers::pi_v<f32> / 2;
    native.scale = 2;
    LegendsGate gate{legendsN64Gate(native)};
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
