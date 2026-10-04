#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"

#include "game/players/CursorAim.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
TEST_CASE("cursor movement is forward backward and strafe relative to aim not camera",
          "[controls][cursor]") {
    const Vec3 position{0};
    const Vec3 aim{10, 5, 0};
    auto move = cursorRelativeMove({{0, 1}, 1}, position, aim, 0);
    CHECK(move.direction.x == Approx(1));
    CHECK(move.direction.y == Approx(0).margin(0.0001));
    move = cursorRelativeMove({{0, -1}, 1}, position, aim, 0);
    CHECK(move.direction.x == Approx(-1));
    move = cursorRelativeMove({{1, 0}, 1}, position, aim, 0);
    CHECK(move.direction.y == Approx(-1));
    move = cursorRelativeMove({{0, 1}, 1}, position, aim, glm::half_pi<f32>());
    CHECK(move.direction.y == Approx(1));
}
TEST_CASE("surface picking still resolves the nearest sloped and moving solid surface",
          "[controls][cursor][collision]") {
    CollisionTriangle triangle;
    triangle.vertices = {Vec3{-10, 0, -10}, Vec3{10, 0, -10}, Vec3{0, 10, 10}};
    triangle.object = 4;
    WorldCollision collision;
    collision.build({triangle});
    auto point = collision.pickSurface({0, 20, 0}, {0, -20, 0});
    REQUIRE(point);
    CHECK(point->y == Approx(5));
    const std::array<s32, 1> moving{4};
    collision.setMovingObjects(moving);
    collision.setObjectTransform(4, glm::translate(Mat4{1}, Vec3{0, 3, 0}));
    point = collision.pickSurface({0, 20, 0}, {0, -20, 0});
    REQUIRE(point);
    CHECK(point->y == Approx(8));
    collision.setSolid(4, false);
    CHECK_FALSE(collision.pickSurface({0, 20, 0}, {0, -20, 0}));
}
TEST_CASE("screen-direction aim keeps the player's height and has a stable centre dead zone",
          "[controls][cursor]") {
    WorldCamera camera;
    camera.position = {0, 10, -10};
    camera.pitch = glm::quarter_pi<f32>();
    const auto clip = WorldCamera::projection(glm::radians(60.0f), 1.5f) * camera.view();
    const Vec3 position{0};
    auto point = cursorAimPoint({0.5f, 0.5f}, clip, position);
    REQUIRE(point);
    CHECK(point->y == Approx(0).margin(0.001));
    CHECK(point->z == Approx(0).margin(0.001));
    CHECK(glm::distance(*point, position) == Approx(0).margin(0.001));
    point = cursorAimPoint({0.501f, 0.5f}, clip, position);
    REQUIRE(point);
    CHECK(*point == position);
    CHECK_FALSE(cursorAimPoint({-0.1f, 0.5f}, clip, position));
    CHECK_FALSE(cursorAimPoint({0.5f, 0.5f}, Mat4{0}, position));
    CHECK_FALSE(cursorAimPoint({std::numeric_limits<f32>::quiet_NaN(), 0.5f}, clip, position));
    CHECK_FALSE(cursorAimPoint({0.5f, 0.5f}, clip, camera.position - camera.forward()));
}
TEST_CASE("a controller does not acquire mouse facing from a stationary menu cursor",
          "[controls][cursor]") {
    Input input;
    CursorInput cursor;
    const PlayBindings bindings;
    input.setPointer({0.5f, 0.5f, true, false});
    PadSnapshot pad;
    pad.connected = true;
    pad.axes[0] = 1;
    input.setPad(0, pad);
    CHECK_FALSE(cursor.update(input, bindings, true, 0));
    input.beginPoll();
    input.setKey(Key::W, true);
    CHECK(cursor.update(input, bindings, true, 0));
    CHECK_FALSE(cursor.update(input, bindings, false, 0));
}

TEST_CASE("screen direction follows the displayed player through camera and viewport changes",
          "[controls][cursor]") {
    for (const auto viewport : {Vec2{640, 448}, Vec2{1920, 1080}, Vec2{3440, 1440}}) {
        for (const f32 yaw : {0.0f, 0.8f, 2.0f}) {
            for (const f32 pitch : {0.2f, 0.8f, 1.5f}) {
                WorldCamera camera;
                camera.yaw = yaw;
                camera.pitch = pitch;
                camera.roll = 0.12f;
                const Vec3 position{4, 7, -3};
                camera.position = position - camera.forward() * 40.0f + camera.right() * 4.0f;
                const auto projection = makeLetterboxProjection(640, 448, viewport.x, viewport.y);
                const auto clip = camera.clipTransform(glm::radians(60.0f), 640, 448, projection);
                for (const auto offset :
                     {Vec3{3, 0, 4}, Vec3{-3, 0, 4}, Vec3{3, 0, -4}, Vec3{-3, 0, -4}}) {
                    const Vec4 projected = clip * Vec4{position + offset, 1};
                    const Vec2 pointer = (Vec2{projected} / projected.w + Vec2{1}) * 0.5f;
                    const auto aim = cursorAimPoint(pointer, clip, position);
                    REQUIRE(aim);
                    CHECK(aim->y == position.y);
                    CHECK(glm::distance(*aim - position, glm::normalize(offset)) ==
                          Approx(0).margin(0.0001));
                }
            }
        }
    }
}

TEST_CASE("mouse direction remains forward above the horizon instead of picking sky or scenery",
          "[controls][cursor]") {
    WorldCamera camera;
    camera.pitch = 0.15f;
    const Vec3 position{0};
    camera.position = -camera.forward() * 20.0f;
    const auto clip = camera.clipTransform(glm::radians(60.0f), 640, 448,
                                           makeLetterboxProjection(640, 448, 1920, 1080));
    for (const f32 y : {0.4f, 0.2f, 0.0f}) {
        const auto aim = cursorAimPoint({0.5f, y}, clip, position);
        REQUIRE(aim);
        CHECK(aim->x == Approx(0).margin(0.0001));
        CHECK(aim->y == position.y);
        CHECK(aim->z == Approx(1));
    }
}
} // namespace
#include <limits>
