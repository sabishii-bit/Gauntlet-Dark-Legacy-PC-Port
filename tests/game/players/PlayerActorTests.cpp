#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/world/WorldCollision.h"

#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"
#include "game/players/PlayerActor.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

CollisionTriangle triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& normal) {
    CollisionTriangle out;
    out.vertices = {a, b, c};
    out.normal = normal;
    return out;
}

/** A 20x20 floor at y = 0 with a wall along x = 5 facing -x, three units tall. */
WorldCollision room() {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 west{-1.0f, 0.0f, 0.0f};
    WorldCollision collision;
    collision.build({
        triangle({-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, up),
        triangle({-10, 0, -10}, {10, 0, 10}, {-10, 0, 10}, up),
        triangle({5, 0, -10}, {5, 3, -10}, {5, 3, 10}, west),
        triangle({5, 0, -10}, {5, 3, 10}, {5, 0, 10}, west),
    });
    return collision;
}

ClassStats warrior() {
    ClassStats stats;
    stats.speedMin = 350.0f;
    stats.speedMax = 750.0f;
    stats.height = 5.0f;
    stats.width = 1.5f;
    stats.collisionY = 2.5f;
    return stats;
}

MoveInput push(f32 x, f32 y, f32 magnitude = 1.0f) {
    return MoveInput{glm::normalize(Vec2{x, y}), magnitude};
}

TEST_CASE("an actor walks at its class speed, facing its heading, relative to the camera",
          "[game][players][actor]") {
    PlayerActor actor;
    CharacterSave save;
    save.name = "AB";
    const ClassStats stats = warrior();
    actor.spawn(2, save, &stats, Vec3{0.0f, 0.5f, 0.0f}, 1.0f);
    REQUIRE(actor.player() == 2);
    REQUIRE(actor.speed() == Approx(5.0f + 0.35f * 7.5f));
    REQUIRE(actor.radius() == Approx(0.75f));
    REQUIRE(actor.height() == Approx(5.0f));
    REQUIRE(actor.followPoint().y == Approx(3.0f));
    REQUIRE_FALSE(actor.moving());

    const WorldCollision collision = room();
    actor.settle(collision);
    REQUIRE(actor.position().y == Approx(0.0f));

    actor.update(push(0.0f, 1.0f), 0.0f, 1.0f, &collision);
    REQUIRE(actor.moving());
    REQUIRE(actor.position().z == Approx(7.625f));
    REQUIRE(actor.position().x == Approx(0.0f).margin(1e-5f));
    REQUIRE(actor.yaw() == Approx(0.0f));

    // With the camera turned around, the same push walks the other way.
    actor.update(push(0.0f, 1.0f), kPi, 1.0f, &collision);
    REQUIRE(actor.position().z == Approx(0.0f).margin(1e-4f));
    REQUIRE(actor.yaw() == Approx(kPi));

    // Half a push walks half as far; letting go stops.
    actor.update(push(0.0f, 1.0f, 0.5f), 0.0f, 1.0f, &collision);
    REQUIRE(actor.position().z == Approx(7.625f * 0.5f));
    actor.update(MoveInput{}, 0.0f, 1.0f, &collision);
    REQUIRE_FALSE(actor.moving());

    // Model space is placed at the feet, turned to the heading.
    const Vec4 nose = actor.transform() * Vec4{0.0f, 0.0f, 1.0f, 1.0f};
    REQUIRE(nose.z == Approx(actor.position().z + 1.0f));
}

TEST_CASE("walls stop an actor and missing floors keep it where it stands",
          "[game][players][actor]") {
    PlayerActor actor;
    const CharacterSave save;
    actor.spawn(0, save, nullptr, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(actor.speed() == Approx(PlayerActor::kMinSpeed));
    const WorldCollision collision = room();
    for (int i = 0; i < 3; ++i) {
        actor.update(push(1.0f, 0.0f), 0.0f, 1.0f, &collision);
    }
    REQUIRE(actor.position().x == Approx(5.0f - actor.radius()).margin(0.01f));
    REQUIRE(actor.position().z == Approx(0.0f).margin(1e-4f));

    actor.spawn(0, save, nullptr, Vec3{-9.5f, 0.0f, 0.0f}, 0.0f);
    actor.update(push(-1.0f, 0.0f), 0.0f, 1.0f, &collision);
    REQUIRE(actor.position().x > -10.0f); // it walks to the floor's edge and no further
    REQUIRE(actor.position().x < -9.5f);

    // Without collision the actor is free to go anywhere.
    const f32 edge = actor.position().x;
    actor.update(push(-1.0f, 0.0f), 0.0f, 1.0f, nullptr);
    REQUIRE(actor.position().x == Approx(edge - 5.0f));
}

} // namespace
