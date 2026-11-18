#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
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

/** The room's Auto Melee and the hero's own Auto Attack control. */
struct Controls {
    bool autoMelee = false;
    bool autoAttack = true;
};

/** An invulnerable warrior on open ground in the Fields, facing east, with the level's own
 * generators and swarm gone and an asleep grunt six units off to its north-east, facing
 * `gruntFacing`. */
struct FieldsGrunt {
    static constexpr s32 kPlayer = 3;
    static constexpr f32 kToGrunt = kPi * 0.25f; ///< the heading from the start to the grunt

    GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    LevelWorld world;
    GameServices context;
    PlayScene scene;
    f64 step = 0;
    Vec3 start{0};
    s32 grunt = -1;

    FieldsGrunt(const std::filesystem::path& root, s32 fps, Controls controls,
                const Vec3& gruntFacing, f32 turbo = 0) {
        config.combat.autoMelee = controls.autoMelee;
        REQUIRE(levels.load(root));
        REQUIRE(world.load(device, root, *levels.byName("G1")));
        context.config = &config;
        context.levels = &levels;
        context.unpackedRoot = root;
        CharacterSave save;
        save.autoAttack = controls.autoAttack;
        save.progress().inventory.addPowerup(powerup::kArmor, armor::kInvulnerable, 0, -1);
        PartyMember member{kPlayer, save};
        member.turbo = turbo;
        const std::array party{member};
        PlayOptions options;
        options.welcome = false;
        options.position = Vec3{18.8f, 0.2f, 0.7f};
        options.yaw = kPi * 0.5f;
        REQUIRE(scene.open(device, context, world, party, options));
        const auto entering = [&] {
            return scene.spawning() || scene.animator(kPlayer)->entering();
        };
        step = 1.0 / fps;
        for (s32 frame = 0; frame < fps * 7 && entering(); ++frame) {
            scene.update(step, {});
        }
        REQUIRE_FALSE(entering());
        start = actor().position();
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
        spawn.position = start + 6.0f * toGrunt();
        spawn.direction = gruntFacing;
        const auto id = enemies.spawn(spawn, {});
        REQUIRE(id);
        grunt = *id;
    }
    FieldsGrunt(const FieldsGrunt&) = delete;
    FieldsGrunt& operator=(const FieldsGrunt&) = delete;
    FieldsGrunt(FieldsGrunt&&) = delete;
    FieldsGrunt& operator=(FieldsGrunt&&) = delete;
    ~FieldsGrunt() { scene.close(); }

    static Vec3 toGrunt() { return {std::sin(kToGrunt), 0, std::cos(kToGrunt)}; }
    const PlayerActor& actor() const { return *scene.actor(kPlayer); }
    Vec3 gruntAt() const { return scene.enemies().positionOf(grunt); }
    /** The original holds the collision centre col_radius, the whole width, off the body. */
    f32 held() const { return actor().reach() + scene.enemies().radiusOf(grunt); }
    /** The port's slide holds the feet a footprint off it. */
    f32 footprint() const { return actor().radius() + scene.enemies().radiusOf(grunt); }

    /** One update with the stick pushed toward `heading`, strafing or charging as asked. */
    void move(f32 heading, bool strafe, bool charge = false) {
        PlayScene::Inputs input{};
        const f32 stick = heading - scene.viewCamera().yaw;
        input[kPlayer].move = MoveInput{Vec2{std::sin(stick), std::cos(stick)}, 1};
        input[kPlayer].strafe = strafe;
        input[kPlayer].chargePressed = charge;
        REQUIRE(scene.update(step, input) == PlayOutcome::Running);
        // Nothing else of the swarm comes near enough to steer the hero.
        const auto close = scene.enemies().within(actor().position(), 8);
        REQUIRE(std::ranges::all_of(close, [&](s32 id) { return id == grunt; }));
    }
};

TEST_CASE("Auto Melee stops a strafing hero dead against an enemy while the hero's own Auto "
          "Attack is on, and otherwise the hero slides along",
          "[game][screens][assets][auto-attack-stop]") {
    // With Auto Attack on, PlayerMotion zeroes a step that would end within col_radius of the body
    // of one of the swarm (0x80082820-0x8008283C). A strafe never forces Auto Attack's swing
    // (motions 9 to 12), so only the stop holds the hero; without the room's Auto Melee, or with
    // the hero's own control off, the port slides it along the body.
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const s32 fps = GENERATE(30, 60);
    const auto controls =
        GENERATE(Controls{false, true}, Controls{true, true}, Controls{true, false});
    const bool stops = controls.autoMelee && controls.autoAttack;
    CAPTURE(fps, controls.autoMelee, controls.autoAttack);
    // One way to strafe, a ninth of a half turn (twenty degrees) past the line to the grunt: the
    // hero would pass 2.05 from its middle.
    constexpr f32 kStrafe = FieldsGrunt::kToGrunt + kPi / 9.0f;
    const Vec3 way{std::sin(kStrafe), 0, std::cos(kStrafe)};
    FieldsGrunt fields{root, fps, controls, -way};
    const PlayerActor& actor = fields.actor();
    const Vec3 start = actor.position();
    for (s32 along = 0; along <= 10; ++along) {
        const Vec3 point = start + static_cast<f32>(along) * way;
        REQUIRE(fields.world.collision().floorAt(point, 0.25f, 0.25f));
        for (usize i = 0; i < fields.scene.barrels().size(); ++i) {
            if (fields.scene.barrels().standing(i)) {
                const auto& barrel = fields.scene.barrels().barrel(i);
                REQUIRE(across(barrel.figure.position(), point) - barrel.radius >
                        actor.reach() + 2.0f);
            }
        }
    }
    const Vec3 at = fields.gruntAt();
    const f32 held = fields.held();
    const f32 footprint = fields.footprint();
    REQUIRE(across(start, at) > held + 2);
    f32 stride = 0;
    f32 nearest = across(start, at);
    std::optional<Vec3> rest;
    std::optional<Vec3> touched;
    for (s32 frame = 0; frame < fps * 3 && !rest && !touched; ++frame) {
        const Vec3 before = actor.position();
        fields.move(kStrafe, true);
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
    if (stops) {
        REQUIRE(rest);
        CAPTURE(across(*rest, at));
        CHECK(across(*rest, at) >= held - 1e-4f);
        CHECK(across(*rest, at) <= held + stride + 1e-4f);
        for (s32 frame = 0; frame < 90; ++frame) {
            fields.move(kStrafe, true);
            nearest = std::min(nearest, across(actor.position(), at));
            CHECK(across(actor.position(), *rest) < 1e-4f);
        }
        CHECK(nearest >= held - 1e-4f);
    } else {
        // It comes inside the original's distance to the footprint and goes on along the body.
        REQUIRE(touched);
        for (s32 frame = 0; frame < 90 && across(actor.position(), *touched) <= 1; ++frame) {
            fields.move(kStrafe, true);
            CHECK(across(actor.position(), at) >= footprint - 1e-3f);
        }
        CHECK(across(actor.position(), *touched) > 1);
        CHECK(nearest < held);
    }
    // Walking straight away frees the hero either way.
    const Vec3 freed = actor.position();
    const Vec3 away = freed - at;
    for (s32 frame = 0; frame < fps / 2; ++frame) {
        fields.move(std::atan2(away.x, away.z), false);
    }
    CHECK(across(actor.position(), freed) > 1);
    CHECK(across(actor.position(), at) > across(freed, at) + 1);
}

TEST_CASE("under Auto Melee a hero's charge at an enemy runs on where a step stops dead",
          "[game][screens][assets][auto-attack-stop][charge]") {
    // Auto Attack zeroes a step into one of the swarm, but a charge into one runs on at three
    // quarters of its pace (0x80082724-0x80082760); the port slides it, so it comes inside the
    // whole width a step is held off by and rams the grunt.
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const s32 fps = GENERATE(30, 60);
    CAPTURE(fps);
    FieldsGrunt fields{root, fps, Controls{true, true}, -FieldsGrunt::toGrunt(), 50.0f};
    const f32 health = fields.scene.enemies().healthOf(fields.grunt);
    fields.move(FieldsGrunt::kToGrunt, false, true);
    REQUIRE(fields.scene.animator(FieldsGrunt::kPlayer)->shoving());
    // Measured against where the grunt stood as each step was taken: a ram throws it back.
    f32 nearest = across(fields.actor().position(), fields.gruntAt());
    for (s32 frame = 0; frame < fps * 2 && fields.scene.animator(FieldsGrunt::kPlayer)->shoving();
         ++frame) {
        const Vec3 at = fields.gruntAt();
        fields.move(FieldsGrunt::kToGrunt, false);
        nearest = std::min(nearest, across(fields.actor().position(), at));
    }
    CAPTURE(nearest, fields.held(), fields.footprint());
    CHECK(nearest < fields.held() - 0.25f);
    CHECK(fields.scene.enemies().healthOf(fields.grunt) < health);
}
} // namespace
