#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/ScriptedFixtures.h"
#include "game/party/PlayerArsenalCheckpoint.h"
#include "game/party/PlayerAttacksCheckpoint.h"
#include "game/party/ScriptedAttacks.h"

namespace gdl::test {
inline std::filesystem::path attackCheckpointAssets() {
    const auto root = scratchDirectory("attack-owner-checkpoint");
    for (const auto* name : {"PLAYERS/WAR/YEL", "MONSTERS/GRU", "WEAPONS"}) {
        const auto dir = root / name;
        std::filesystem::create_directories(dir);
        writeTextFile(dir / "body.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(dir / "objects.json", R"({"objects":[
            {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
        writeFile(dir / "skin.png", kTinyPng);
        writeTextFile(dir / "textures.json", R"({"bitmaps":[
            {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        convertModelFixture(dir);
    }
    writeTextFile(root / "MONSTERS/GRU/animations.json", R"({"trees":[{"name":"GRU1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":60,"rate":30}]}]})");
    writeTextFile(root / "PLAYERS/WAR/YEL/animations.json", R"({"trees":[{"name":"WAR_YEL",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[]}]})");
    std::filesystem::create_directories(root / "PLAYERS/WAR/ANIM");
    writeTextFile(root / "PLAYERS/WAR/ANIM/animations.json", R"({"trees":[{"name":"WAR",
        "nodes":[{"name":"BODY","parent":-1,"position":[0,0,0]}],"sequences":[
        {"name":"READY","frames":60,"rate":30},
        {"name":"ATTPWRB","frames":60,"rate":30},
        {"name":"ATTPWRC","frames":60,"rate":30},
        {"name":"ATTBREATHE","frames":30,"rate":30},
        {"name":"ATTBREATHER","frames":30,"rate":30}]}]})");
    std::string trees = R"({"trees":[)";
    for (const auto* name : {"BURST", "FIREBREATHE", "ACIDBREATHE", "ELECBREATHE", "BOSS_BREATHE",
                             "EXPRING", "MP_FIRE", "MP_ELEC", "MP_LIGHT", "MP_ACID", "MS_FIRE",
                             "MS_ELEC", "MS_LIGHT", "MS_ACID", "MAGICHEALTH", "BLOCKFX"}) {
        if (trees.back() != '[') {
            trees += ',';
        }
        trees += std::string{R"({"name":")"} + name + R"(",
            "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"ACTIVE","frames":60,"rate":30}]})";
    }
    trees += "]}";
    writeTextFile(root / "WEAPONS/animations.json", trees);
    std::filesystem::create_directories(root / "pdata");
    writeTextFile(root / "pdata/WAR.json", R"({"height":6,"width":2,
        "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
        "moves":{"turboB":0,"turboC1":1},"moveEffects":[{"tree":"BURST"}],"moveStrikes":[
        {"type":4,"startFrame":1,"radius":12,"arc":-1,"delay":0.1,"amount":50,"effect":0},
        {"type":2,"startFrame":1,"hitRadius":10,"arc":-1,"offset":[0,3,2],
         "speedMin":1,"speedMax":1,"maxTime":6,"amount":70,"flags":64,
         "damageType":1048576}]})");
    return root;
}

struct AttackCheckpointFixture {
    std::filesystem::path root = attackCheckpointAssets();
    FakeRenderDevice device;
    game::ClassDataSet classes;
    game::LevelWorld world;
    ItemArchive weapons;
    TextureAnimator textures;
    game::EffectTrees prototypes;
    std::vector<game::EffectTrees::PoolResource> resources;
    game::EffectTrees effects;
    game::LevelSoundscape audio;
    AmbientDimmer dimmer;
    game::PlayerArsenal arsenal;
    game::LevelOpponents opponents;
    game::LevelFixtures fixtures;
    game::PlayerAttacks attacks;
    std::array<game::PlayerRuntime, 1> players;
    std::array<game::PlayerAttacksCheckpoint::Actor, 1> actors{{{3, 0}}};
    std::array<s32, 1> slots{{3}};
    // Native used/shattered potion impacts retain MissileImpact's SPARKS
    // default even though the area-damage phase does not play that effect.
    std::array<game::PlayerMissiles::Resource, 1> missileResources{
        {{1, &game::MissileSpec::of(0), nullptr, nullptr}}};
    std::vector<s32> helps;
    FixtureScript fixtureEvents;
    ScriptedFixtures fixtureHost{fixtureEvents};
    ScriptedAttacks attackHost;
    game::PlayerAttacks::Targets targets{opponents, fixtures, fixtureHost, attackHost};

    explicit AttackCheckpointFixture(bool artwork = true) {
        REQUIRE(classes.load(root / "pdata"));
        if (artwork) {
            REQUIRE(weapons.load(root / "WEAPONS"));
            textures.bind(weapons.trees.textureAnimations(), weapons.textures, device);
            for (usize index = 0; index < weapons.trees.size(); ++index) {
                const auto& tree = weapons.trees.tree(static_cast<u32>(index));
                REQUIRE(prototypes.start(device, weapons, tree.name, {}) != 0);
            }
            for (usize index = 0; index < prototypes.count(); ++index) {
                resources.push_back({static_cast<u32>(index + 1),
                                     &weapons,
                                     {},
                                     &textures,
                                     &prototypes.effect(index)});
            }
        }
        arsenal.bind({device,
                      classes,
                      weapons,
                      world.collision(),
                      effects,
                      audio,
                      nullptr,
                      {},
                      false,
                      false,
                      artwork ? &weapons : nullptr});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        players[0].actor.spawn(3, {}, classes.stats(0), {}, 0);
        players[0].figure = game::PlayerFigure::load(device, root, players[0].actor.save(), false);
        REQUIRE(players[0].figure);
        fixtureEvents.help = [this](s32 id, usize actor) {
            CHECK(actor == 0);
            helps.push_back(id);
            return true;
        };
    }
    AttackCheckpointFixture(const AttackCheckpointFixture&) = delete;
    AttackCheckpointFixture& operator=(const AttackCheckpointFixture&) = delete;
    ~AttackCheckpointFixture() {
        attacks.clear();
        arsenal.clear();
        opponents.close();
    }
    game::PlayerAttacksCheckpoint::State state() const {
        const auto result =
            game::PlayerAttacksCheckpoint::capture(attacks, actors, missileResources);
        REQUIRE(result);
        return *result;
    }
    game::EffectTrees::PoolCheckpoint effectState() const {
        const auto result = effects.checkpointPool(resources);
        if (!result) {
            for (usize index = 0; index < effects.count(); ++index) {
                const auto& effect = effects.effect(index);
                const auto state = effects.checkpoint(effect.id);
                CAPTURE(effect.name, effect.id, effect.lived, effect.secondsLeft);
                REQUIRE(state);
                CHECK(state->animation.valid());
                CHECK(state->particles.valid());
                CHECK(state->trails.valid());
                CHECK(state->valid());
                const auto resource = std::ranges::find_if(resources, [&](const auto& candidate) {
                    return candidate.prototype != nullptr &&
                           candidate.prototype->name == effect.name;
                });
                REQUIRE(resource != resources.end());
                const auto& prototype = *resource->prototype;
                CHECK(prototype.archive == effect.archive);
                CHECK(prototype.tree == effect.tree);
                CHECK(prototype.device == effect.device);
                CHECK(prototype.lenders == effect.lenders);
                CHECK(prototype.emitParticles == effect.emitParticles);
                CHECK(prototype.missingObjectsAreEmpty == effect.missingObjectsAreEmpty);
                CHECK(prototype.particleTextureSlot == effect.particleTextureSlot);
                CHECK(prototype.trails.canRestore(state->trails));
                CHECK(prototype.particles.canRestore(state->particles));
            }
        }
        REQUIRE(result);
        return *result;
    }
    void restore(const game::PlayerAttacksCheckpoint::State& attackState,
                 const game::EffectTrees::PoolCheckpoint& visualState) {
        auto a = game::PlayerAttacksCheckpoint::prepare(attacks, attackState, actors, visualState,
                                                        missileResources);
        auto b = effects.prepareRestorePool(visualState, resources);
        REQUIRE(a);
        REQUIRE(b);
        REQUIRE(game::PlayerAttacksCheckpoint::canCommit(attacks, *a));
        REQUIRE(effects.canCommit(*b));
        REQUIRE(game::PlayerAttacksCheckpoint::commit(attacks, std::move(*a)));
        REQUIRE(effects.commitRestore(std::move(*b)));
    }
};
} // namespace gdl::test
