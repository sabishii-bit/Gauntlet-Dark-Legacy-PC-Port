#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerImpact.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/HazardSurfaces.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

CollisionTriangle triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& normal,
                           s32 object) {
    CollisionTriangle out;
    out.vertices = {a, b, c};
    out.normal = normal;
    out.object = object;
    return out;
}

TEST_CASE("a surface's flags name what it does to a body", "[game][world][hazards]") {
    CHECK_FALSE(HazardSurfaces::harmOf(0));
    CHECK_FALSE(HazardSurfaces::harmOf(0x1004));
    const auto burn = HazardSurfaces::harmOf(0x10000);
    REQUIRE(burn);
    CHECK(burn->damage == 5.0f);
    CHECK(burn->impact == 0);
    CHECK_FALSE(burn->jolts);
    const auto knock = HazardSurfaces::harmOf(0x20000);
    REQUIRE(knock);
    CHECK(knock->damage == 10.0f);
    CHECK(knock->impact == PlayerImpact::kKnockBack);
    for (const u32 kind : {0x30000U, 0x40000U, 0x50000U}) {
        const auto fell = HazardSurfaces::harmOf(kind);
        REQUIRE(fell);
        CHECK(fell->damage == 15.0f);
        CHECK(fell->impact == PlayerImpact::kKnockDown);
        CHECK(fell->jolts);
    }
    CHECK(HazardSurfaces::harmOf(0x60000)->damage == 5.0f);
    // Enemies take the knocking kind as a burn with a knock, and nothing from the sixth.
    CHECK(HazardSurfaces::enemyHarmOf(0x10000)->damage == 5.0f);
    CHECK(HazardSurfaces::enemyHarmOf(0x20000)->damage == 5.0f);
    CHECK(HazardSurfaces::enemyHarmOf(0x20000)->impact == PlayerImpact::kKnockBack);
    CHECK(HazardSurfaces::enemyHarmOf(0x40000)->damage == 15.0f);
    CHECK_FALSE(HazardSurfaces::enemyHarmOf(0x60000));
    // What a trigger drives hurts only when it is also marked to.
    CHECK_FALSE(HazardSurfaces::harmOf(0x30000 | HazardSurfaces::kTriggered));
    CHECK(HazardSurfaces::harmOf(0x30000 | HazardSurfaces::kTriggered |
                                 HazardSurfaces::kHarmsWhenTriggered));
}

TEST_CASE("a body is hurt by a harmful wall it is against or a harmful floor it stands on",
          "[game][world][hazards]") {
    const auto dir = test::scratchDirectory("hazard-surfaces");
    // A harmless floor (0), a group that knocks back (1) whose wall (2) inherits it, and a
    // burning floor (3).
    writeTextFile(dir / "world.json", R"({
  "objects": [
    {"name": "FLOOR", "position": [0, 0, 0], "flags": 4, "next": 1, "child": -1},
    {"name": "ROLLERS", "position": [0, 0, 0], "flags": 131072, "next": 3, "child": 2},
    {"name": "ROLLER", "position": [0, 0, 0], "flags": 0, "next": -1, "child": -1},
    {"name": "EMBERS", "position": [0, 0, 0], "flags": 65540, "next": -1, "child": -1}
  ],
  "animations": [], "particles": [], "locators": [], "itemInfos": [], "itemInstances": []
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    HazardSurfaces hazards;
    hazards.bind(layout);
    CHECK_FALSE(hazards.harmOfObject(0));
    REQUIRE(hazards.harmOfObject(2));
    CHECK(hazards.harmOfObject(2)->impact == PlayerImpact::kKnockBack);
    CHECK(hazards.harmful() == 3);

    const Vec3 up{0.0f, 1.0f, 0.0f};
    WorldCollision collision;
    collision.build({
        triangle({-20, 0, -20}, {0, 0, 20}, {0, 0, -20}, up, 0),
        triangle({-20, 0, -20}, {-20, 0, 20}, {0, 0, 20}, up, 0),
        triangle({0, 0, -20}, {0, 0, 20}, {20, 0, 20}, up, 3),
        triangle({0, 0, -20}, {20, 0, 20}, {20, 0, -20}, up, 3),
        // A wall along x at z 10, facing back towards -z.
        triangle({-20, 0, 10}, {-5, 8, 10}, {-5, 0, 10}, {0, 0, -1}, 2),
        triangle({-20, 0, 10}, {-20, 8, 10}, {-5, 8, 10}, {0, 0, -1}, 2),
    });
    // Out in the open on the harmless floor: nothing.
    CHECK_FALSE(hazards.touching(collision, Vec3{-10, 0, 0}, 0.75f, 5.0f));
    // Against the roller wall: knocked back, away from it.
    const auto wall = hazards.touching(collision, Vec3{-10, 0, 9.3f}, 0.75f, 5.0f);
    REQUIRE(wall);
    CHECK(wall->object == 2);
    CHECK(wall->harm.damage == 10.0f);
    CHECK(wall->away.z == Approx(-1.0f));
    // A slide can finish beyond the wall it hit. FloorFX still receives that hit,
    // rather than relying on finding the same wall at the corrected position.
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {-10, 0, 9}, 0);
    actor.slide({8, 0, 2}, &collision);
    REQUIRE_FALSE(actor.wallContacts().empty());
    CHECK_FALSE(hazards.touching(collision, actor.position(), actor.radius(), actor.height()));
    const auto retained = hazards.touching(collision, actor.position(), actor.radius(),
                                           actor.height(), actor.wallContacts());
    REQUIRE(retained);
    CHECK(retained->object == 2);
    CHECK(retained->harm.damage == 10.0f);
    actor.clearWallContacts();
    CHECK_FALSE(hazards.touching(collision, actor.position(), actor.radius(), actor.height(),
                                 actor.wallContacts()));
    // On the embers: burned, with nowhere in particular to be thrown.
    const auto floor = hazards.touching(collision, Vec3{10, 0, 0}, 0.75f, 5.0f);
    REQUIRE(floor);
    CHECK(floor->object == 3);
    CHECK(floor->harm.damage == 5.0f);
    CHECK(floor->away == Vec3{0.0f});
}

TEST_CASE("the fields keep their harmful floors and rollers", "[game][world][hazards][assets]") {
    const auto file = test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(file.parent_path()));
    HazardSurfaces hazards;
    hazards.bind(layout);
    CHECK(hazards.harmful() == 16); // eight that burn, eight that fell
}

TEST_CASE("Nightmare turbines retain harmful contacts", "[turbine-hazards][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELJ6/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("J6")));
    world.startTriggers({});
    for (usize object = 0; object < world.layout().objects().size(); ++object) {
        if (!world.layout().objects()[object].name.starts_with("J6SPINNY")) {
            continue;
        }
        CAPTURE(object, world.layout().objects()[object].name,
                world.hazards().flagsOf(static_cast<s32>(object)));
        REQUIRE(world.hazards().harmOfObject(static_cast<s32>(object)));
        const Vec3 centre{world.scene().worldTransform(object)[3]};
        usize blocked = 0;
        usize missed = 0;
        for (s32 x = -20; x <= 20; ++x) {
            for (s32 z = -20; z <= 20; ++z) {
                const Vec3 at =
                    centre + Vec3{static_cast<f32>(x) * 0.5f, -2, static_cast<f32>(z) * 0.5f};
                for (const Vec3 direction :
                     {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1}}) {
                    std::vector<WallContact> contacts;
                    const Vec3 to = world.collision().sweepWalls(
                        at, at + direction, 0.6f, at.y + PlayerActor::kFootClearance,
                        at.y + 5 - PlayerActor::kFootClearance, &contacts);
                    for (const auto& contact : contacts) {
                        if (contact.object != static_cast<s32>(object)) {
                            continue;
                        }
                        ++blocked;
                        if (!world.hazards().touching(world.collision(), to, 0.6f, 5)) {
                            ++missed;
                        }
                        const auto harm =
                            world.hazards().touching(world.collision(), to, 0.6f, 5, contacts);
                        REQUIRE(harm);
                        CHECK(harm->harm.damage == 5.0f);
                        if (blocked == 1) {
                            PlayerRuntime player;
                            player.actor.spawn(0, {}, nullptr, to, 0);
                            player.actor.save().progress().health = 1000;
                            PlayerHealth health;
                            const PlayerHealth::Events events{.block = [](f32, f32) {},
                                                              .sound = [](std::string_view) {},
                                                              .cry = [](std::string_view) {},
                                                              .named = [](std::string_view, f32) {},
                                                              .learnBlock = [] {},
                                                              .vibrate = {}};
                            health.hurt(player, harm->harm.damage, HurtKind::Blow, true, false,
                                        1.0f, events, PlayerImpact{harm->harm.impact, harm->away});
                            CHECK(player.actor.save().health() == 995);
                        }
                    }
                }
            }
        }
        INFO("wall hits=" << blocked << "; final-position reprobe misses=" << missed);
        CHECK(blocked > 0);
    }
}

TEST_CASE("native trolleys and minecarts knock players down without dragging them along",
          "[game][world][hazards][moving-hazards][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG2/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    struct Crossing {
        const char* level;
        Vec3 position;
    };
    // Authored G2RAIL1's northern bend and I1MINECART#1's upper straight.
    for (const Crossing crossing : {Crossing{"G2", {-35.4368f, 1.265625f, -190.734f}},
                                    Crossing{"I1", {51.953125f, 40.953125f, -270.25f}}}) {
        CAPTURE(crossing.level);
        LevelWorld world;
        REQUIRE(world.load(device, root, *catalog.byName(crossing.level)));
        world.startTriggers({});
        ItemArchive weapons;
        EffectTrees effects;
        LevelSoundscape audio;
        std::array<PlayerRuntime, 1> players;
        std::array<PlayInput, 1> inputs;
        auto& runtime = players[0];
        auto& actor = runtime.actor;
        actor.spawn(0, {}, nullptr, crossing.position, 0);
        actor.save().progress().health = 1000;
        runtime.figure = PlayerFigure::load(device, root, actor.save(), false);
        REQUIRE(runtime.figure);
        LevelOpponents opponents;
        opponents.open({device, world, weapons, effects, audio, root, 1, true}, players);
        PartyMotion::Events events;
        events.perform = [](usize, PartyMotion::Action) {};
        events.select = [](usize, const SelectorInput&, s32) {};
        events.advanceTurbo = [](usize, s32, f32) {};
        events.resolveMovement = [&](usize, const Vec3& from, const Vec3& to) {
            return opponents.resolveMovement(actor, from, to);
        };
        const PlayerHealth::Events healthEvents{.block = [](f32, f32) {},
                                                .sound = [](std::string_view) {},
                                                .cry = [](std::string_view) {},
                                                .named = [](std::string_view, f32) {},
                                                .learnBlock = [] {},
                                                .vibrate = {}};
        PlayerHealth health;
        s32 hits = 0;
        s32 overlapping = 0;
        s32 recovering = 0;
        f32 mostTravel = 0;
        for (s32 frame = 0; frame < 270; ++frame) {
            world.update(1.0f / 30);
            // Exercise the actual post-opponent wall check, not just the low-level
            // hazard sampler. An incoming cart must not move an idle body itself.
            if (world.hazards().touching(world.collision(), crossing.position, actor.radius(),
                                         actor.height())) {
                ++overlapping;
                const Vec3 rechecked =
                    opponents.resolveMovement(actor, crossing.position, crossing.position);
                CAPTURE(frame, rechecked.x, rechecked.y, rechecked.z);
                CHECK(rechecked == crossing.position);
            }
            PartyMotion::step(players, inputs, false, 0, 2, 1.0f / 30, world.collision(), events);
            mostTravel = std::max(mostTravel, glm::distance(actor.position(), crossing.position));
            runtime.surfaceGap = std::max(runtime.surfaceGap - 1.0f / 30, 0.0f);
            if (runtime.figure->animator().reacting()) {
                ++recovering;
                CHECK_FALSE(PlayerHealth::canTakeSurfaceDamage(runtime));
            }
            if (runtime.surfaceGap > 0 || !PlayerHealth::canTakeSurfaceDamage(runtime)) {
                continue;
            }
            const auto touch =
                world.hazards().touching(world.collision(), actor.position(), actor.radius(),
                                         actor.height(), actor.wallContacts());
            if (!touch) {
                continue;
            }
            runtime.surfaceGap = 1;
            REQUIRE(touch->harm.impact == PlayerImpact::kKnockDown);
            const s32 before = actor.save().health();
            health.hurt(runtime, touch->harm.damage, HurtKind::Blow, true, false, 1, healthEvents,
                        {touch->harm.impact, touch->away});
            hits += actor.save().health() < before ? 1 : 0;
        }
        CHECK(overlapping > 0);
        CHECK(recovering > 0);
        CHECK(hits == 1);
        CHECK(actor.save().health() == 985);
        CHECK(mostTravel < 5); // Ordinary knockback, not G2's former 27-unit cart ride.
        opponents.close();
    }
}

} // namespace
