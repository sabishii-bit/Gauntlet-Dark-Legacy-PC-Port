#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/world/WorldCollision.h"

#include "TestSupport.h"
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

TEST_CASE("an action that holds the feet still lets the body turn", "[game][players][actor]") {
    PlayerActor actor;
    const CharacterSave save;
    const ClassStats stats = warrior();
    actor.spawn(0, save, &stats, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(actor.facing().z == Approx(1.0f));
    actor.update(MoveInput{Vec2{1.0f, 0.0f}, 1.0f}, 0.0f, 0.5f, nullptr, 0.0f);
    REQUIRE(actor.position() == Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE_FALSE(actor.moving());
    REQUIRE(actor.facing().x == Approx(1.0f));
    REQUIRE(actor.facing().z == Approx(0.0f).margin(1e-5f));
    // A speed powerup's bonus adds straight onto the pace.
    const f32 plain = actor.speed();
    actor.setPaceBonus(2.0f);
    REQUIRE(actor.speed() == Approx(plain + 2.0f));
    actor.setPaceBonus(100.0f); // a speed powerup never takes it past the range's most
    REQUIRE(actor.speed() == PlayerActor::kMaxSpeed);
    actor.setPaceBonus(0.0f);
    // Half its pace is half the ground.
    actor.update(MoveInput{Vec2{1.0f, 0.0f}, 1.0f}, 0.0f, 1.0f, nullptr, 0.5f);
    REQUIRE(actor.position().x == Approx(actor.speed() * 0.5f));
}

TEST_CASE("walls stop an actor and missing floors keep it where it stands",
          "[game][players][actor]") {
    PlayerActor actor;
    const CharacterSave save;
    actor.spawn(0, save, nullptr, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(actor.speed() == Approx(PlayerActor::kMinSpeed));
    const WorldCollision collision = room();
    for (s32 i = 0; i < 3; ++i) {
        actor.update(push(1.0f, 0.0f), 0.0f, 1.0f, &collision);
    }
    REQUIRE(actor.position().x == Approx(5.0f - actor.radius()).margin(0.01f));
    REQUIRE(actor.position().z == Approx(0.0f).margin(1e-4f));

    actor.spawn(0, save, nullptr, Vec3{-9.5f, 0.0f, 0.0f}, 0.0f);
    actor.update(push(-1.0f, 0.0f), 0.0f, 1.0f, &collision);
    REQUIRE(actor.position().x >= -10.0f); // it walks to the floor's edge and no further
    REQUIRE(actor.position().x < -9.5f);

    // Without collision the actor is free to go anywhere.
    const f32 edge = actor.position().x;
    actor.update(push(-1.0f, 0.0f), 0.0f, 1.0f, nullptr);
    REQUIRE(actor.position().x == Approx(edge - 5.0f));
}

TEST_CASE("a player slides along a cliff without stepping down it or stopping its tangent",
          "[game][players][actor][cliff]") {
    f32 angle = 0;
    SECTION("axis aligned") {}
    SECTION("diagonal") {
        angle = kPi / 4;
    }
    const Mat4 rotation = glm::rotate(Mat4{1}, angle, Vec3{0, 1, 0});
    const auto point = [&](Vec3 p) { return Vec3(rotation * Vec4(p, 1)); };
    WorldCollision floor;
    floor.build(
        {triangle(point({-20, 0, -20}), point({0, 0, -20}), point({0, 0, 20}), {0, 1, 0}),
         triangle(point({-20, 0, -20}), point({0, 0, 20}), point({-20, 0, 20}), {0, 1, 0}),
         triangle(point({0, -10, -20}), point({20, -10, -20}), point({20, -10, 20}), {0, 1, 0}),
         triangle(point({0, -10, -20}), point({20, -10, 20}), point({0, -10, 20}), {0, 1, 0})});
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, point({-0.1f, 0, -5}), 0);
    for (s32 frame = 0; frame < 120; ++frame) {
        actor.update(push(1, 1), angle, 1.0f / 60, &floor);
    }
    const Vec3 local = Vec3(glm::inverse(rotation) * Vec4(actor.position(), 1));
    CHECK(local.x <= PlayerActor::kFloorEdgeReach + 0.001f);
    CHECK(local.x > -0.2f);
    CHECK(local.z > 1);
    CHECK(local.y == 0);
}

TEST_CASE("cliff sliding cannot pass through a wall or cling to a removed floor",
          "[game][players][actor][cliff]") {
    auto collision = room();
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, Vec3{4, 0, 9.8f}, 0);
    for (s32 frame = 0; frame < 120; ++frame) {
        actor.update(push(1, 1), 0, 1.0f / 60, &collision);
    }
    CHECK(actor.position().x <= 5 - actor.radius() + 0.001f);
    CHECK(actor.position().z <= 10 + PlayerActor::kFloorEdgeReach);
    // No remaining floor means the separate falling path must handle it.
    collision.clear();
    CHECK_FALSE(collision.slideAlongFloor({0, 0, 0}, {0.2f, 0, 0.2f}, 1.5f, 3, 0.03f));
}

TEST_CASE("a strafing character steps where it is sent without turning to it",
          "[game][players][actor]") {
    PlayerActor actor;
    const CharacterSave save;
    actor.spawn(0, save, nullptr, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    MoveInput sideways;
    sideways.direction = Vec2{1.0f, 0.0f};
    sideways.magnitude = 1.0f;
    REQUIRE(PlayerActor::headingOf(sideways, 0.0f) == Catch::Approx(1.5707964f));
    actor.update(sideways, 0.0f, 0.5f, nullptr, 1.0f, true);
    REQUIRE(actor.position().x > 1.0f);
    REQUIRE(actor.yaw() == 0.0f); // still facing the way it was
    actor.update(sideways, 0.0f, 0.5f, nullptr);
    REQUIRE(actor.yaw() == Catch::Approx(1.5707964f));
}

TEST_CASE("a knock slides the body along the ground, never through a wall or off an edge",
          "[game][players][actor][knockback]") {
    PlayerActor actor;
    const CharacterSave save;
    actor.spawn(0, save, nullptr, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    const WorldCollision collision = room();
    actor.slide(Vec3{0.0f, 3.0f, 2.0f}, &collision);
    CHECK(actor.position() == Vec3{0.0f, 0.0f, 2.0f}); // along the ground only
    CHECK(actor.yaw() == 0.0f);                        // a slide does not turn it
    actor.slide(Vec3{20.0f, 0.0f, 0.0f}, &collision);
    CHECK(actor.position().x == Approx(5.0f - actor.radius()).margin(0.01f));
    actor.place(Vec3{-9.5f, 0.0f, 0.0f});
    actor.slide(Vec3{-5.0f, 0.0f, 0.0f}, &collision);
    CHECK(actor.position().x >= -10.0f);
    CHECK(actor.position().y == 0.0f);
    actor.turnTo(1.0f);
    CHECK(actor.yaw() == 1.0f);
}

TEST_CASE("a body sinks to a floor gone lower at sixteen a second, and rides one that rose",
          "[game][players][actor][falling]") {
    PlayerActor actor;
    const CharacterSave save;
    actor.spawn(0, save, nullptr, Vec3{0.0f, 10.0f, 0.0f}, 0.0f);
    const WorldCollision collision = room();
    // Ten over its floor: it sinks, no faster than sixteen a second, and lands.
    CHECK(actor.fall(0.25f, collision));
    CHECK(actor.position().y == Approx(10.0f - 4.0f));
    CHECK(actor.fall(0.25f, collision));
    CHECK(actor.position().y == Approx(2.0f));
    CHECK(actor.fall(0.25f, collision));
    CHECK(actor.position().y == 0.0f);
    CHECK_FALSE(actor.fall(0.25f, collision));
    CHECK(actor.position().y == 0.0f);
    // A floor a little above it lifts it at once.
    actor.place(Vec3{0.0f, -1.0f, 0.0f});
    CHECK_FALSE(actor.fall(1.0f / 30.0f, collision));
    CHECK(actor.position().y == 0.0f);
    // With no floor at all under it, it keeps sinking.
    actor.place(Vec3{50.0f, 0.0f, 0.0f});
    CHECK(actor.fall(1.0f, collision));
    CHECK(actor.position().y == Approx(-PlayerActor::kFallSpeed));
    CHECK(collision.lowest() == 0.0f);
}

TEST_CASE("walking crosses small cracks between separately packed slope triangles",
          "[game][players][actor][terrain-seams]") {
    // Separate collision faces need not share bit-identical endpoints. A point-only
    // floor ray can fall between them despite the feet straddling both faces.
    const Vec3 normal = glm::normalize(Vec3{-0.5f, 1, 0});
    WorldCollision collision;
    collision.build({
        triangle({-4, -2, -4}, {0, 0, -4}, {0, 0, 4}, normal),
        triangle({-4, -2, -4}, {0, 0, 4}, {-4, -2, 4}, normal),
        triangle({0.02f, 0.01f, -4}, {4, 2, -4}, {4, 2, 4}, normal),
        triangle({0.02f, 0.01f, -4}, {4, 2, 4}, {0.02f, 0.01f, 4}, normal),
    });
    for (const f32 sign : {-1.0f, 1.0f}) {
        PlayerActor actor;
        actor.spawn(0, CharacterSave{}, nullptr, {-sign, -sign * 0.5f, 0}, 0);
        for (s32 tick = 0; tick < 400; ++tick) {
            const Vec3 before = actor.position();
            actor.update(push(sign, 0, 0.03f), 0, 1.0f / 30.0f, &collision);
            REQUIRE((actor.position().x - before.x) * sign > 0.004f);
            REQUIRE(actor.position().y == Approx(actor.position().x * 0.5f).margin(0.02f));
        }
    }
}

TEST_CASE("a body footprint can step onto a nearby higher landing without walking off cliffs",
          "[game][players][actor][terrain-seams]") {
    const Vec3 up{0, 1, 0};
    WorldCollision collision;
    collision.build({triangle({-10, 0, -4}, {0, 0, -4}, {0, 0, 4}, up),
                     triangle({-10, 0, -4}, {0, 0, 4}, {-10, 0, 4}, up),
                     triangle({0.4f, 0.7f, -4}, {10, 0.7f, -4}, {10, 0.7f, 4}, up),
                     triangle({0.4f, 0.7f, -4}, {10, 0.7f, 4}, {0.4f, 0.7f, 4}, up)});
    for (const f32 direction : {-1.0f, 1.0f}) {
        PlayerActor actor;
        const Vec3 start = direction > 0 ? Vec3{-2, 0, 0} : Vec3{2, 0.7f, 0};
        actor.spawn(0, {}, nullptr, start, 0);
        for (s32 tick = 0; tick < 60; ++tick) {
            actor.update(push(direction, 0), 0, 1.0f / 60, &collision);
            actor.fall(1.0f / 60, collision);
        }
        CAPTURE(direction, actor.position().x, actor.position().y);
        CHECK(actor.position().x * direction > 2.5f);
        CHECK(actor.position().y == Approx(direction > 0 ? 0.7f : 0));
    }
}

TEST_CASE("a footprint does not bridge a broad gap or climb an unreachable landing",
          "[game][players][actor][cliff]") {
    f32 gap = 2;
    f32 height = 0;
    SECTION("broad gap") {}
    SECTION("landing too high") {
        gap = 0.4f;
        height = 2;
    }
    SECTION("landing too low") {
        gap = 0.4f;
        height = -4;
    }
    WorldCollision collision;
    const Vec3 up{0, 1, 0};
    collision.build({triangle({-10, 0, -4}, {0, 0, -4}, {0, 0, 4}, up),
                     triangle({-10, 0, -4}, {0, 0, 4}, {-10, 0, 4}, up),
                     triangle({gap, height, -4}, {10, height, -4}, {10, height, 4}, up),
                     triangle({gap, height, -4}, {10, height, 4}, {gap, height, 4}, up)});
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {-2, 0, 0}, 0);
    for (s32 tick = 0; tick < 120; ++tick) {
        actor.update(push(1, 0), 0, 1.0f / 60, &collision);
        actor.fall(1.0f / 60, collision);
    }
    CHECK(actor.position().x <= PlayerActor::kFloorEdgeReach);
    CHECK(actor.position().y == 0);
}

TEST_CASE("the tower's authored stair ramps allow continuous uphill and downhill movement",
          "[game][players][actor][terrain-seams][assets]") {
    const auto dir = test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    WorldCollision collision;
    REQUIRE(collision.load(dir, layout));
    // Cross the join of L1STAIRS_LI#20 / L1STAIRS_LINE07, on the ramp's interior.
    for (const f32 sign : {-1.0f, 1.0f}) {
        PlayerActor actor;
        actor.spawn(0, CharacterSave{}, nullptr, {-18, -3.9f, -4.4f - sign * 0.5f}, 0);
        actor.settle(collision);
        for (s32 tick = 0; tick < 1000; ++tick) {
            const Vec3 before = actor.position();
            actor.update(push(0, sign, 0.006f), 0, 1.0f / 30.0f, &collision);
            actor.fall(1.0f / 30.0f, collision);
            INFO("direction " << sign << " tick " << tick << " at " << before.x << ", " << before.y
                              << ", " << before.z);
            REQUIRE((actor.position().z - before.z) * sign > 0.0009f);
        }
    }
}

TEST_CASE("pushing against opposing faces of a thin wall cannot select its far side",
          "[game][players][actor][push-wall]") {
    WorldCollision collision;
    const Vec3 up{0, 1, 0};
    collision.build({triangle({-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, up),
                     triangle({-10, 0, -10}, {10, 0, 10}, {-10, 0, 10}, up),
                     triangle({5, 0, -10}, {5, 5, -10}, {5, 5, 10}, {-1, 0, 0}),
                     triangle({5, 0, -10}, {5, 5, 10}, {5, 0, 10}, {-1, 0, 0}),
                     triangle({5.1f, 0, -10}, {5.1f, 5, 10}, {5.1f, 5, -10}, {1, 0, 0}),
                     triangle({5.1f, 0, -10}, {5.1f, 0, 10}, {5.1f, 5, 10}, {1, 0, 0})});
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {4, 0, 0}, 0);
    actor.slide({4, 0, 1}, &collision);
    CHECK(actor.position().x <= 5 - actor.radius() + 0.001f);
    CHECK(actor.position().z == Approx(1).margin(0.001f));
    actor.place({6, 0, 0});
    actor.slide({-4, 0, 1}, &collision);
    CHECK(actor.position().x >= 5.1f + actor.radius() - 0.001f);
    CHECK(actor.position().z == Approx(1).margin(0.001f));
}

} // namespace
