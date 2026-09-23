#include <catch2/catch_test_macros.hpp>

#include "game/players/PlayerCapture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

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
