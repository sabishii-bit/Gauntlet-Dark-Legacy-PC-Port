#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/world/WorldCamera.h"

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
TEST_CASE("cursor ray picks the nearest sloped and moving solid surface",
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
TEST_CASE("cursor unprojection follows the rendered camera and falls back over empty ground",
          "[controls][cursor]") {
    WorldCamera camera;
    camera.position = {0, 10, -10};
    camera.pitch = glm::quarter_pi<f32>();
    const auto clip = WorldCamera::projection(glm::radians(60.0f), 1.5f) * camera.view();
    const WorldCollision empty;
    auto point = cursorAimPoint({0.5f, 0.5f}, clip, empty, 0);
    REQUIRE(point);
    CHECK(point->y == Approx(0).margin(0.001));
    CHECK(point->z == Approx(0).margin(0.001));
    CHECK_FALSE(cursorAimPoint({-0.1f, 0.5f}, clip, empty, 0));
    CHECK_FALSE(cursorAimPoint({0.5f, 0.5f}, Mat4{0}, empty, 0));
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

TEST_CASE("mouse picking round-trips a surface through a widescreen rendered camera",
          "[controls][cursor]") {
    WorldCamera camera;
    camera.position = {0, 10, -10};
    camera.pitch = glm::quarter_pi<f32>();
    const auto projection = makeLetterboxProjection(640, 448, 1920, 1080);
    const auto clip = camera.clipTransform(glm::radians(60.0f), 640, 448, projection);
    const Vec3 target{3, 0, 4};
    const Vec4 projected = clip * Vec4{target, 1};
    const Vec2 pointer = (Vec2{projected} / projected.w + Vec2{1}) * 0.5f;
    const WorldCollision collision;
    const auto picked = cursorAimPoint(pointer, clip, collision, target.y);
    REQUIRE(picked);
    CHECK(glm::distance(*picked, target) == Approx(0).margin(0.001));
}
} // namespace
