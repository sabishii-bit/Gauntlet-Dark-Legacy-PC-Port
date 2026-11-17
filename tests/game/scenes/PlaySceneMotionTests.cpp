#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** How far apart two points stand along the ground. */
f32 across(const Vec3& a, const Vec3& b) {
    return glm::length(Vec2{a.x - b.x, a.z - b.z});
}

TEST_CASE("Auto Melee stops a strafing hero dead against an enemy, and without it the hero slides "
          "along",
          "[game][screens][assets][auto-attack-stop]") {
    // With Auto Attack on, PlayerMotion zeroes a step that would end within col_radius of the body
    // of one of the swarm (0x80082820-0x8008283C). A strafe never forces Auto Attack's swing
    // (motions 9 to 12), so only the stop holds the hero; without Auto Melee the port slides it
    // along the body.
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const s32 fps = GENERATE(30, 60);
    const bool autoMelee = GENERATE(false, true);
    CAPTURE(fps, autoMelee);
    GameConfig config;
    config.combat.autoMelee = autoMelee;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameServices context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    constexpr s32 kPlayer = 3;
    CharacterSave save;
    save.autoAttack = true;
    save.progress().inventory.addPowerup(powerup::kArmor, armor::kInvulnerable, 0, -1);
    const std::array party{PartyMember{kPlayer, save}};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{18.8f, 0.2f, 0.7f};
    options.yaw = kPi * 0.5f;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const auto entering = [&] { return scene.spawning() || scene.animator(kPlayer)->entering(); };
    const f64 step = 1.0 / fps;
    for (s32 frame = 0; frame < fps * 7 && entering(); ++frame) {
        scene.update(step, {});
    }
    REQUIRE_FALSE(entering());
    const auto& actor = *scene.actor(kPlayer);
    const Vec3 start = actor.position();
    // An asleep grunt six units off on open ground, and one way to strafe, a ninth of a half
    // turn (twenty degrees) past the line to it: the hero would pass 2.05 from its middle.
    constexpr f32 kToGrunt = kPi * 0.25f;
    constexpr f32 kStrafe = kToGrunt + kPi / 9.0f;
    const Vec3 way{std::sin(kStrafe), 0, std::cos(kStrafe)};
    for (s32 along = 0; along <= 10; ++along) {
        const Vec3 point = start + static_cast<f32>(along) * way;
        REQUIRE(world.collision().floorAt(point, 0.25f, 0.25f));
        for (usize i = 0; i < scene.barrels().size(); ++i) {
            if (scene.barrels().standing(i)) {
                const auto& barrel = scene.barrels().barrel(i);
                REQUIRE(across(barrel.figure.position(), point) - barrel.radius >
                        actor.reach() + 2.0f);
            }
        }
    }
    auto& enemies = scene.enemies();
    REQUIRE(enemies.within(start, 12).empty());
    REQUIRE(scene.generators().within(start, 8).empty());
    // The level's own generators and swarm go, so that none of them comes to steer the hero.
    for (s32 id = 0; std::cmp_less(id, scene.generators().count()); ++id) {
        if (scene.generators().standing(id)) {
            scene.generators().strike(id, 1.0e9f, -1);
        }
    }
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (enemies.alive(id)) {
            EnemyHit hit;
            hit.damage = 1.0e9f;
            enemies.hurt(id, hit);
        }
    }
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.asleep = true;
    spawn.position = start + 6.0f * Vec3{std::sin(kToGrunt), 0, std::cos(kToGrunt)};
    spawn.direction = -way;
    const auto grunt = enemies.spawn(spawn, {});
    REQUIRE(grunt);
    const Vec3 at = enemies.positionOf(*grunt);
    // The original holds the collision centre col_radius, the whole width, off the body; the
    // port's slide holds the feet a footprint off it.
    const f32 held = actor.reach() + enemies.radiusOf(*grunt);
    const f32 footprint = actor.radius() + enemies.radiusOf(*grunt);
    REQUIRE(across(start, at) > held + 2);
    const auto move = [&](f32 heading, bool strafe) {
        PlayScene::Inputs input{};
        const f32 stick = heading - scene.viewCamera().yaw;
        input[kPlayer].move = MoveInput{Vec2{std::sin(stick), std::cos(stick)}, 1};
        input[kPlayer].strafe = strafe;
        REQUIRE(scene.update(step, input) == PlayOutcome::Running);
        // Nothing else of the swarm comes near enough to steer the hero.
        const auto close = enemies.within(actor.position(), 8);
        REQUIRE(std::ranges::all_of(close, [&](s32 id) { return id == *grunt; }));
    };
    f32 stride = 0;
    f32 nearest = across(start, at);
    std::optional<Vec3> rest;
    std::optional<Vec3> touched;
    for (s32 frame = 0; frame < fps * 3 && !rest && !touched; ++frame) {
        const Vec3 before = actor.position();
        move(kStrafe, true);
        const f32 moved = across(actor.position(), before);
        const f32 gap = across(actor.position(), at);
        nearest = std::min(nearest, gap);
        if (gap > held + 1) {
            stride = std::max(stride, moved);
        }
        if (stride > 0 && moved < 1e-4f) {
            rest = actor.position();
        }
        // A slide's tangent carries the feet a little out again by the frame's end.
        if (gap <= footprint + 0.05f) {
            touched = actor.position();
        }
    }
    REQUIRE(stride > 0);
    CAPTURE(stride, nearest, held, footprint);
    if (autoMelee) {
        REQUIRE(rest);
        CAPTURE(across(*rest, at));
        CHECK(across(*rest, at) >= held - 1e-4f);
        CHECK(across(*rest, at) <= held + stride + 1e-4f);
        for (s32 frame = 0; frame < 90; ++frame) {
            move(kStrafe, true);
            nearest = std::min(nearest, across(actor.position(), at));
            CHECK(across(actor.position(), *rest) < 1e-4f);
        }
        CHECK(nearest >= held - 1e-4f);
    } else {
        // It comes inside the original's distance to the footprint and goes on along the body.
        REQUIRE(touched);
        for (s32 frame = 0; frame < 90 && across(actor.position(), *touched) <= 1; ++frame) {
            move(kStrafe, true);
            CHECK(across(actor.position(), at) >= footprint - 1e-3f);
        }
        CHECK(across(actor.position(), *touched) > 1);
        CHECK(nearest < held);
    }
    // Walking straight away frees the hero either way.
    const Vec3 freed = actor.position();
    const Vec3 away = freed - at;
    for (s32 frame = 0; frame < fps / 2; ++frame) {
        move(std::atan2(away.x, away.z), false);
    }
    CHECK(across(actor.position(), freed) > 1);
    CHECK(across(actor.position(), at) > across(freed, at) + 1);
    scene.close();
}
} // namespace
