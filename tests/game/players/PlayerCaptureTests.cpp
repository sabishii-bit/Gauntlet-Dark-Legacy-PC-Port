#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/players/PlayerCapture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a grab below its takeoff floor lands without selecting an overhead deck",
          "[game][players][capture]") {
    const f32 dt = GENERATE(1.0f / 15, 1.0f / 30, 1.0f / 60);
    WorldCollision collision;
    collision.build({
        {{0, 1, 0}, {{{-20, 0, -20}, {20, 0, -20}, {20, 0, 20}}}},
        {{0, 1, 0}, {{{-20, 0, -20}, {20, 0, 20}, {-20, 0, 20}}}},
        {{0, 1, 0}, {{{-20, 10, -20}, {20, 10, -20}, {20, 10, 20}}}},
        {{0, 1, 0}, {{{-20, 10, -20}, {20, 10, 20}, {-20, 10, 20}}}},
    });
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    PlayerCapture capture;
    capture.attach(1, true, glm::translate(Mat4{1}, Vec3{0, 8, 0}), actor);
    // Animation, not physics, drags the body through its supporting floor. Repeated
    // held updates must not replace the remembered floor with the hand's height.
    capture.attach(1, true, glm::translate(Mat4{1}, Vec3{0, -2, 0}), actor);
    REQUIRE(actor.position().y < 0);
    capture.release({0, -100, 1000}, 70);
    const auto damage = capture.update(dt, actor, collision);
    REQUIRE(damage);
    CHECK(*damage == 70);
    CHECK(actor.position().y == 0);
    CHECK_FALSE(capture.active());
    CHECK_FALSE(capture.update(dt, actor, collision)); // damage is delivered once
}

TEST_CASE("capture recovery requires a real floor and resets for the next grab",
          "[game][players][capture]") {
    WorldCollision collision;
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    PlayerCapture capture;
    capture.attach(1, true, glm::translate(Mat4{1}, Vec3{0, -2, 0}), actor);
    capture.release({0, -100, 0}, 70);
    CHECK_FALSE(capture.update(1.0f / 30, actor, collision));
    CHECK(capture.flying()); // an empty map does not create an invisible landing plane
    capture.clear();
    actor.place({0, -10, 0});
    collision.build({
        {{0, 1, 0}, {{{-20, 0, -20}, {20, 0, -20}, {20, 0, 20}}}},
        {{0, 1, 0}, {{{-20, 0, -20}, {20, 0, 20}, {-20, 0, 20}}}},
    });
    capture.attach(1, true, glm::translate(Mat4{1}, Vec3{0, -12, 0}), actor);
    capture.release({0, -100, 0}, 70);
    CHECK_FALSE(capture.update(1.0f / 30, actor, collision));
    CHECK(actor.position().y < -10); // the previous grab's upper floor is not reused
}

TEST_CASE("capture owns its hand pose and ignores release without an attachment",
          "[game][players][capture][yeti]") {
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    PlayerCapture capture;
    const WorldCollision collision;
    capture.release(Vec3{100}, 100);
    REQUIRE_FALSE(capture.active());
    REQUIRE_FALSE(capture.update(1, actor, collision).has_value());
    Mat4 hand = glm::translate(Mat4{1}, Vec3{3, 10, 7});
    capture.attach(5, true, hand, actor);
    REQUIRE(capture.held());
    REQUIRE(capture.owner() == 5);
    REQUIRE(capture.boss());
    const Mat4 saved = *capture.body();
    hand[3] = Vec4{0};
    REQUIRE(*capture.body() == saved);
    const Vec3 held = actor.position();
    REQUIRE_FALSE(capture.update(1, actor, collision).has_value());
    REQUIRE(actor.position() == held);
    capture.release(Vec3{0, -100, 1000}, 100);
    REQUIRE(capture.flying());
    REQUIRE_FALSE(capture.body().has_value());
    REQUIRE_FALSE(capture.update(0, actor, collision).has_value());
    REQUIRE(actor.position() == held);
    capture.clear();
    REQUIRE_FALSE(capture.active());
    REQUIRE(capture.owner() == -1);
    REQUIRE_FALSE(capture.boss());
}
} // namespace
