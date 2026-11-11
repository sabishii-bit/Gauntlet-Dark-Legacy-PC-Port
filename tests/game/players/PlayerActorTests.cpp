#include <numbers>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Progression.h"
#include "game/level/LevelWorld.h"
#include "game/party/FloorRiding.h"
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

/** A 20x20 floor at y = 0 with a wall along x = 5 facing -x, five units tall. */
WorldCollision room() {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 west{-1.0f, 0.0f, 0.0f};
    WorldCollision collision;
    collision.build({
        triangle({-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, up),
        triangle({-10, 0, -10}, {10, 0, 10}, {-10, 0, 10}, up),
        triangle({5, 0, -10}, {5, 5, -10}, {5, 5, 10}, west),
        triangle({5, 0, -10}, {5, 5, 10}, {5, 0, 10}, west),
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

TEST_CASE("Sumner walks at the master's pace on his base class's record",
          "[game][players][actor][sumner]") {
    // PlayerUpdateAtts gives the hidden master 999 in every attribute before the class
    // table is consulted: his pace is the greatest, 5 + 0.999 * 7.5, not the record's.
    PlayerActor actor;
    CharacterSave save;
    save.name = "AB";
    save.character = kSumnerClass;
    const ClassStats stats = warrior();
    actor.spawn(0, save, &stats, Vec3{0.0f}, 0.0f);
    REQUIRE(actor.speed() == Approx(5.0f + 0.999f * 7.5f));
    REQUIRE(actor.radius() == Approx(0.75f)); // the body is the record's
    const StatBlock block = characterStats(stats, save);
    REQUIRE(block.speed() == 999);
    REQUIRE(block.magic() == 999);
    save.character = 0;
    REQUIRE(characterStats(stats, save).speed() == 350);
}

TEST_CASE("attention anchors remain separate from collision and camera-follow heights",
          "[game][players][actor][name-anchor]") {
    PlayerActor actor;
    ClassStats stats = warrior();
    stats.attentionY = 4.4f;
    actor.spawn(0, {}, &stats, Vec3{1, 2, 3}, 0);
    CHECK(actor.attentionPoint() == Vec3{1, 6.4f, 3});
    CHECK(actor.followPoint() == Vec3{1, 4.5f, 3});
    actor.place(Vec3{4, 5, 6});
    CHECK(actor.attentionPoint() == Vec3{4, 9.4f, 6});
    CHECK(actor.followPoint() == Vec3{4, 7.5f, 6});
    actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    CHECK(actor.attentionPoint().y == PlayerActor::kDefaultFollowHeight);
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
    // PlayerWallCollide's sphere is as wide as the class: the wall stops it a width out.
    REQUIRE(actor.position().x == Approx(5.0f - actor.reach()).margin(0.01f));
    REQUIRE(actor.position().z == Approx(0.0f).margin(1e-4f));

    // A drop beyond the floor's edge stops it a width short of the edge (PlayerCollideFloor
    // looks a width ahead and takes the overhang off the step).
    actor.spawn(0, save, nullptr, Vec3{-8.0f, 0.0f, 0.0f}, 0.0f);
    for (s32 i = 0; i < 60; ++i) {
        actor.update(push(-1.0f, 0.0f), 0.0f, 1.0f / 60.0f, &collision);
    }
    REQUIRE(actor.position().x == Approx(-10.0f + actor.reach()).margin(0.01f));

    // Without collision the actor is free to go anywhere.
    const f32 edge = actor.position().x;
    actor.update(push(-1.0f, 0.0f), 0.0f, 1.0f, nullptr);
    REQUIRE(actor.position().x == Approx(edge - 5.0f));
}

TEST_CASE("a step into a wall still aims a step past where the wall holds the body",
          "[game][players][actor]") {
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    const WorldCollision collision = room();
    REQUIRE(actor.stepAim() == actor.position());
    for (s32 i = 0; i < 90; ++i) {
        actor.update(push(1.0f, 0.0f), 0.0f, 1.0f / 30.0f, &collision);
    }
    // PlayerMotion sweeps items at oldpos + dpos, before the wall clips dpos: the body
    // waits a width out, its aim a step beyond.
    REQUIRE(actor.position().x == Approx(5.0f - actor.reach()).margin(0.01f));
    REQUIRE(actor.stepAim().x == Approx(actor.position().x + actor.speed() / 30.0f).margin(1e-4f));
    REQUIRE(actor.stepAim().z == Approx(0.0f).margin(1e-4f));
    // Standing still, it aims at its own feet; a free step lands where it aimed.
    actor.update(MoveInput{}, 0.0f, 1.0f / 30.0f, &collision);
    REQUIRE(actor.stepAim() == actor.position());
    actor.update(push(-1.0f, 0.0f), 0.0f, 1.0f / 30.0f, &collision);
    REQUIRE(actor.stepAim().x == Approx(actor.position().x).margin(1e-4f));
    // What carries or shoves the body afterwards carries its aim with it.
    const Vec3 aim = actor.stepAim();
    actor.carry(actor.position() + Vec3{0.0f, 2.0f, 1.0f});
    REQUIRE(actor.stepAim() == aim + Vec3{0.0f, 2.0f, 1.0f});
    actor.place(Vec3{1.0f, 0.0f, 1.0f});
    REQUIRE(actor.stepAim() == actor.position());
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
    // Starting nearer the drop than its width, it backs off along its way until a width
    // ahead of it is floor again (the look-ahead's overhang comes off the step), and goes
    // on along the edge.
    CHECK(local.x == Approx(-actor.reach() / std::sqrt(2.0f)).margin(0.05f));
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
    CHECK(actor.position().x <= 5 - actor.reach() + 0.001f);
    // Against a wall, PlayerCollideFloor looks no further ahead: the body reaches the very
    // edge and is held on it.
    CHECK(actor.position().z <= 10 + 0.01f);
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
    CHECK(actor.position().x == Approx(5.0f - actor.reach()).margin(0.01f));
    actor.place(Vec3{-9.5f, 0.0f, 0.0f});
    actor.slide(Vec3{-5.0f, 0.0f, 0.0f}, &collision);
    CHECK(actor.position().x == -9.5f); // already nearer the edge than its width: held
    CHECK(actor.position().y == 0.0f);
    actor.turnTo(1.0f);
    CHECK(actor.yaw() == 1.0f);
}

TEST_CASE("a step's walk and slide go together, held to the step's limit along each axis",
          "[game][players][actor][knockback][move-limit]") {
    PlayerActor actor;
    const CharacterSave save;
    actor.spawn(0, save, nullptr, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    const WorldCollision collision = room();
    constexpr f32 kSeconds = 1.0f / 30.0f;
    const f32 walked = actor.speed() * kSeconds;
    // The walk and a slide the same way, more than the limit together, go no further than it.
    actor.update(push(0.0f, 1.0f), 0.0f, kSeconds, &collision, 1.0f, false, Vec3{0.0f, 0.0f, 0.4f},
                 0.25f);
    CHECK(actor.position().z == Approx(0.25f));
    CHECK(actor.stepAim().z == Approx(0.25f));
    CHECK(actor.yaw() == 0.0f);
    // A slide across the walk is held apart from it, an axis at a time, not by its length.
    actor.place(Vec3{0.0f});
    actor.update(push(0.0f, 1.0f), 0.0f, kSeconds, &collision, 1.0f, false, Vec3{0.4f, 0.0f, 0.0f},
                 0.25f);
    CHECK(actor.position().x == Approx(0.25f));
    CHECK(actor.position().z == Approx(walked));
    // The slide alone still goes, out of walls, and nothing lifts it.
    actor.place(Vec3{0.0f});
    actor.update(MoveInput{}, 0.0f, kSeconds, &collision, 1.0f, false, Vec3{20.0f, 3.0f, 0.0f});
    CHECK(actor.position().x == Approx(5.0f - actor.reach()).margin(0.01f));
    CHECK(actor.position().y == 0.0f);
    CHECK_FALSE(actor.moving());
    // A held action's walk goes nowhere, the slide still carries it, and it turns to the stick.
    actor.place(Vec3{0.0f});
    actor.update(push(1.0f, 0.0f), 0.0f, kSeconds, &collision, 0.0f, false, Vec3{0.0f, 0.0f, 0.1f});
    CHECK(actor.position().x == Approx(0.0f));
    CHECK(actor.position().z == Approx(0.1f));
    CHECK(actor.yaw() == Approx(kPi / 2));
    // Lifted off its floor, it sinks back at the native pace.
    actor.lift(1.0f);
    CHECK(actor.position().y == Approx(1.0f));
    CHECK(actor.stepAim().y == Approx(1.0f));
    actor.fall(kSeconds, collision);
    CHECK(actor.position().y == Approx(1.0f - PlayerActor::kFallSpeed * kSeconds));
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
            actor.fall(1.0f / 30.0f, collision);
            REQUIRE((actor.position().x - before.x) * sign > 0.004f);
            REQUIRE(actor.position().y == Approx(actor.position().x * 0.5f).margin(0.02f));
        }
    }
}

TEST_CASE("walking down a step uses the same descent limit as a lowered floor",
          "[game][players][actor][walking-height]") {
    const Vec3 up{0, 1, 0};
    WorldCollision collision;
    collision.build({triangle({-10, 0, -4}, {0, 0, -4}, {0, 0, 4}, up),
                     triangle({-10, 0, -4}, {0, 0, 4}, {-10, 0, 4}, up),
                     triangle({0, -2.5f, -4}, {10, -2.5f, -4}, {10, -2.5f, 4}, up),
                     triangle({0, -2.5f, -4}, {10, -2.5f, 4}, {0, -2.5f, 4}, up)});
    for (const f32 seconds : {1.0f / 30, 1.0f / 60, 1.0f / 120}) {
        PlayerActor actor;
        actor.spawn(0, {}, nullptr, {-1, 0, 0}, 0);
        for (s32 tick = 0; tick < static_cast<s32>(2 / seconds); ++tick) {
            const Vec3 before = actor.position();
            actor.update(push(1, 0), 0, seconds, &collision);
            actor.fall(seconds, collision);
            INFO("seconds " << seconds << " tick " << tick);
            REQUIRE(before.y - actor.position().y <= PlayerActor::kFallSpeed * seconds + 0.0001f);
        }
        CHECK(actor.position().x > 2);
        CHECK(actor.position().y == Approx(-2.5f));
    }
}

TEST_CASE("a footprint contact cannot walk alongside a raised ledge in empty space",
          "[game][players][actor][walking-height]") {
    const Vec3 up{0, 1, 0};
    WorldCollision collision;
    collision.build({triangle({-4, 0, -4}, {4, 0, -4}, {4, 0, 0}, up),
                     triangle({-4, 0, -4}, {4, 0, 0}, {-4, 0, 0}, up),
                     triangle({-4, 1, 0}, {0, 1, 0}, {0, 1, 10}, up),
                     triangle({-4, 1, 0}, {0, 1, 10}, {-4, 1, 10}, up)});
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {0.4f, 0, -1}, 0);
    for (s32 tick = 0; tick < 30; ++tick) {
        actor.update(push(0, 1), 0, 1.0f / 30, &collision);
        actor.fall(1.0f / 30, collision);
        if (actor.position().z > 0.5f) {
            REQUIRE(collision.floorAt(actor.position(), 0.05f, 0.05f, PlayerActor::kFloorEdgeReach)
                        .has_value());
        }
    }
    CHECK(actor.position().z > 2);
    for (s32 tick = 0; tick < 30; ++tick) {
        actor.update(push(0, -1), 0, 1.0f / 30, &collision);
        actor.fall(1.0f / 30, collision);
    }
    CHECK(actor.position().z < 0);
    CHECK(actor.position().y == 0);
}

TEST_CASE("uphill travel accounts for the height gained instead of racing up the ramp",
          "[game][players][actor][walking-height]") {
    const Vec3 normal = glm::normalize(Vec3{-1, 1, 0});
    WorldCollision collision;
    collision.build({triangle({-20, -20, -10}, {20, 20, -10}, {20, 20, 10}, normal),
                     triangle({-20, -20, -10}, {20, 20, 10}, {-20, -20, 10}, normal)});
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    constexpr f32 kSeconds = 1.0f / 30;
    actor.update(push(1, 0), 0, kSeconds, &collision);
    actor.fall(kSeconds, collision);
    CHECK(actor.position().x == Approx(actor.speed() * kSeconds / std::sqrt(2.0f)).margin(0.001f));
    CHECK(actor.position().y == Approx(actor.position().x).margin(0.001f));
}

TEST_CASE("subdividing a displacement cannot climb several full steps at once",
          "[game][players][actor][walking-height]") {
    std::vector<CollisionTriangle> faces;
    for (s32 step = 0; step < 4; ++step) {
        const f32 x = static_cast<f32>(step) * 0.3f;
        const f32 y = static_cast<f32>(step) * 2.5f;
        faces.push_back(triangle({x, y, -3}, {x + 0.3f, y, -3}, {x + 0.3f, y, 3}, {0, 1, 0}));
        faces.push_back(triangle({x, y, -3}, {x + 0.3f, y, 3}, {x, y, 3}, {0, 1, 0}));
    }
    WorldCollision collision;
    collision.build(std::move(faces));
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {0.1f, 0, 0}, 0);
    actor.slide({2, 0, 0}, &collision);
    CHECK(actor.position().y <= PlayerActor::kStepUp);
}

TEST_CASE("retail's extra step tolerance is restricted to the current floor object",
          "[game][players][actor][walking-height]") {
    for (const bool sameObject : {false, true}) {
        const Vec3 up{0, 1, 0};
        std::vector faces{triangle({-10, 0, -4}, {0, 0, -4}, {0, 0, 4}, up),
                          triangle({-10, 0, -4}, {0, 0, 4}, {-10, 0, 4}, up),
                          triangle({0, -3.5f, -4}, {10, -3.5f, -4}, {10, -3.5f, 4}, up),
                          triangle({0, -3.5f, -4}, {10, -3.5f, 4}, {0, -3.5f, 4}, up)};
        faces[0].object = faces[1].object = 1;
        faces[2].object = faces[3].object = sameObject ? 1 : 2;
        WorldCollision collision;
        collision.build(std::move(faces));
        PlayerActor actor;
        actor.spawn(0, {}, nullptr, {-1, 0, 0}, 0);
        for (s32 tick = 0; tick < 60; ++tick) {
            actor.update(push(1, 0), 0, 1.0f / 30, &collision);
            actor.fall(1.0f / 30, collision);
        }
        CAPTURE(sameObject, actor.position().x, actor.position().y);
        if (sameObject) {
            CHECK(actor.position().x > 1);
            CHECK(actor.position().y == Approx(-3.5f));
        } else {
            CHECK(actor.position().x <= PlayerActor::kFloorEdgeReach);
            CHECK(actor.position().y == 0);
        }
    }
}

TEST_CASE("a floor above the collision centre cannot be climbed or pull an idle player up",
          "[game][players][actor][walking-height]") {
    const Vec3 up{0, 1, 0};
    WorldCollision collision;
    collision.build({triangle({-10, 0, -4}, {10, 0, -4}, {10, 0, 4}, up),
                     triangle({-10, 0, -4}, {10, 0, 4}, {-10, 0, 4}, up),
                     triangle({0, 2.75f, -4}, {10, 2.75f, -4}, {10, 2.75f, 4}, up),
                     triangle({0, 2.75f, -4}, {10, 2.75f, 4}, {0, 2.75f, 4}, up)});
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {-1, 0, 0}, 0);
    for (s32 tick = 0; tick < 30; ++tick) {
        actor.update(push(1, 0), 0, 1.0f / 30, &collision);
        actor.fall(1.0f / 30, collision);
        CHECK(actor.position().y == 0);
    }
    CHECK(actor.position().x > 1);
}

TEST_CASE("a body bridges a gap narrower than itself onto a nearby higher landing",
          "[game][players][actor][terrain-seams]") {
    // A gap under the point with floor of the same step a width beyond it is bridged by
    // the feet, where the original's rule, meeting no such seam in its own levels, would
    // hold the body a width short of the gap's edge.
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
        height = 3.1f;
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

TEST_CASE("a body can reboard a lowered deck whose side is below its wall volume",
          "[game][players][actor][lift-reboard]") {
    constexpr f32 kDeck = 1.9375f; // G1's lowered second lift above its north ground
    const Vec3 up{0, 1, 0};
    const Vec3 west{-1, 0, 0};
    auto surfaces = std::vector{
        triangle({-10, 0, -4}, {10, 0, -4}, {10, 0, 4}, up),
        triangle({-10, 0, -4}, {10, 0, 4}, {-10, 0, 4}, up),
        triangle({0, kDeck, -4}, {10, kDeck, -4}, {10, kDeck, 4}, up),
        triangle({0, kDeck, -4}, {10, kDeck, 4}, {0, kDeck, 4}, up),
    };
    f32 wallHeight = kDeck;
    bool blocked = false;
    SECTION("the deck side is beneath the native wall sphere") {}
    SECTION("a taller wall still blocks the approach") {
        wallHeight = 3;
        blocked = true;
    }
    surfaces.push_back(triangle({0, 0, -4}, {0, wallHeight, -4}, {0, wallHeight, 4}, west));
    surfaces.push_back(triangle({0, 0, -4}, {0, wallHeight, 4}, {0, 0, 4}, west));
    WorldCollision collision;
    collision.build(std::move(surfaces));
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {-2, 0, 0}, 0);
    for (s32 tick = 0; tick < 60; ++tick) {
        actor.update(push(1, 0), 0, 1.0f / 60, &collision);
        actor.fall(1.0f / 60, collision);
    }
    if (blocked) {
        CHECK(actor.position().x <= -actor.radius() + 0.001f);
        CHECK(actor.position().y == 0);
    } else {
        CHECK(actor.position().x > 2.5f);
        CHECK(actor.position().y == Approx(kDeck));
    }
}

TEST_CASE("clearing a low side does not permit walking beyond an unsupported edge",
          "[game][players][actor][lift-reboard][cliff]") {
    const Vec3 up{0, 1, 0};
    const Vec3 west{-1, 0, 0};
    WorldCollision collision;
    collision.build({triangle({-10, 0, -4}, {0, 0, -4}, {0, 0, 4}, up),
                     triangle({-10, 0, -4}, {0, 0, 4}, {-10, 0, 4}, up),
                     triangle({0, 0, -4}, {0, 1.9f, -4}, {0, 1.9f, 4}, west),
                     triangle({0, 0, -4}, {0, 1.9f, 4}, {0, 0, 4}, west)});
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {-2, 0, 0}, 0);
    for (s32 tick = 0; tick < 60; ++tick) {
        actor.update(push(1, 0), 0, 1.0f / 60, &collision);
        actor.fall(1.0f / 60, collision);
    }
    CHECK(actor.position().x == Approx(-actor.reach()).margin(0.01f)); // a width short
    CHECK(actor.position().y == 0);
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

TEST_CASE("a body on the plane of a one-sided railing at a floor's edge can walk back in",
          "[game][players][actor][railing]") {
    // The Shipyard's crow's nest rails its floor with boards facing outward. Pulled onto the
    // floor's edge against a wall, a body stands on the boards' plane; a face it stands on
    // is behind it, not in front, so it is free to step back and never wedged there.
    const Vec3 up{0, 1, 0};
    WorldCollision collision;
    collision.build({triangle({-10, 0, -10}, {0, 0, -10}, {0, 0, 10}, up),
                     triangle({-10, 0, -10}, {0, 0, 10}, {-10, 0, 10}, up),
                     triangle({0, 0, -10}, {0, 4, 10}, {0, 4, -10}, {1, 0, 0}),
                     triangle({0, 0, -10}, {0, 0, 10}, {0, 4, 10}, {1, 0, 0})});
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    for (s32 tick = 0; tick < 30; ++tick) {
        actor.update(push(-1, 0), 0, 1.0f / 60, &collision);
        actor.fall(1.0f / 60, collision);
    }
    CHECK(actor.position().x < -2);
    // In front of the boards there is no floor: the body cannot leave by them either.
    actor.place({-0.5f, 0, 0});
    for (s32 tick = 0; tick < 30; ++tick) {
        actor.update(push(1, 0), 0, 1.0f / 60, &collision);
        actor.fall(1.0f / 60, collision);
    }
    CHECK(actor.position().x <= 0.0f);
    CHECK(actor.position().y == 0);
}

TEST_CASE("K4 elevator riders cannot step onto the passing gold platform through its corner",
          "[game][players][actor][lift-floor-inheritance][k4-lift-corner][assets]") {
    const s32 hz = GENERATE(30, 60, 120);
    const f32 seconds = 1.0f / static_cast<f32>(hz);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELK4/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("K4");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    world.startTriggers({});
    world.updateTriggers(0, {});
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    std::array<PlayerRuntime, 1> party;
    auto& player = party[0];
    player.actor.spawn(0, {}, classes.stats(0), {-7.625f, 16.328125f, -136.625f}, 0);
    player.actor.settle(world.collision());
    FloorRiding::land(party, 0, player.actor.position(), world.collision());
    REQUIRE(player.floor.object == 232);
    // BRIDGEPAD 441 animates root K4PLAT3 (231), while K4WHATA (232)
    // carries its floor. Shoot target 610 raises the adjacent gold floor 451.
    REQUIRE(world.layout().objects()[232].parent == 231);
    REQUIRE(world.layout().objects()[451].parent == 450);
    bool passedGoldHeight = false;
    s32 heldFrames = 0;
    for (s32 frame = 0; frame < 8 * hz; ++frame) {
        world.update(seconds);
        FloorRiding::carry(player, world.collision());
        const Vec3 from = player.actor.position();
        const bool locked = world.collision().ownFloorExitBlocked(231);
        if (locked) {
            ++heldFrames;
            REQUIRE(world.collision().floorExitBlocked(232));
            player.actor.update(push(-1, -1), 0, seconds, &world.collision());
        }
        FloorRiding::land(party, 0, from, world.collision());
        player.actor.fall(seconds, world.collision());
        if (locked) {
            CHECK(player.floor.object == 232);
            CHECK(player.actor.floorObject() == 232);
            passedGoldHeight |= player.actor.position().y > 32 && player.actor.position().y < 35;
        }
        const std::array visitors{TriggerVisitor{.position = player.actor.position(),
                                                 .radius = player.actor.reach(),
                                                 .floorObject = player.floor.object,
                                                 .height = player.actor.height()}};
        world.updateTriggers(seconds, visitors);
    }
    CHECK(heldFrames > hz);
    CHECK(passedGoldHeight);
}

TEST_CASE("an actor's pace follows its record's level once told to look again",
          "[game][player-actor]") {
    const ClassStats stats = warrior();
    const CharacterSave save;
    PlayerActor actor;
    actor.spawn(0, save, &stats, Vec3{0, 0, 0}, 0.0f);
    PlayerActor bare;
    bare.spawn(1, save, nullptr, Vec3{0, 0, 0}, 0.0f);
    const f32 novice = actor.speed();
    CHECK(novice > bare.speed());
    actor.save().progress().experience = levelExperience(kMaxLevel);
    CHECK(actor.speed() == novice); // nothing changes until it is told
    actor.restat(&stats);
    CHECK(actor.speed() > novice);
    actor.restat(nullptr);
    CHECK(actor.speed() == bare.speed());
}

} // namespace
