#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/NativeStateResources.h"
#include "game/level/ScriptedFixtures.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/ScriptedAttacks.h"

namespace gdl::test {
inline std::filesystem::path attackSceneAssets() {
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

/** One player's attack owners bound to a synthetic scene: the world, weapons, effects,
 * opponents and fixtures they borrow, with scripted hosts for what the attacks report. */
struct AttackSceneFixture {
    std::filesystem::path root = attackSceneAssets();
    FakeRenderDevice device;
    game::ClassDataSet classes;
    game::LevelWorld world;
    ItemArchive weapons;
    TextureAnimator textures;     ///< the weapons' texture animations, bound as a scene binds them
    game::EffectTrees prototypes; ///< every weapons tree started once, so its textures exist
    game::EffectTrees effects;
    game::LevelSoundscape audio;
    AmbientDimmer dimmer;
    game::PlayerArsenal arsenal;
    game::LevelOpponents opponents;
    game::LevelFixtures fixtures;
    game::PlayerAttacks attacks;
    std::array<game::PlayerRuntime, 1> players;
    std::vector<s32> helps;
    FixtureScript fixtureEvents;
    ScriptedFixtures fixtureHost{fixtureEvents};
    ScriptedAttacks attackHost;
    game::PlayerAttacks::Targets targets{opponents, fixtures, fixtureHost, attackHost};

    explicit AttackSceneFixture(bool artwork = true) {
        REQUIRE(classes.load(root / "pdata"));
        if (artwork) {
            REQUIRE(weapons.load(root / "WEAPONS"));
            textures.bind(weapons.trees.textureAnimations(), weapons.textures, device);
            for (usize index = 0; index < weapons.trees.size(); ++index) {
                const auto& tree = weapons.trees.tree(static_cast<u32>(index));
                REQUIRE(prototypes.start(device, weapons, tree.name, {}) != 0);
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
    AttackSceneFixture(const AttackSceneFixture&) = delete;
    AttackSceneFixture& operator=(const AttackSceneFixture&) = delete;
    ~AttackSceneFixture() {
        attacks.clear();
        arsenal.clear();
        opponents.close();
    }
    /** The attack owners' native digests, to compare one moment with another. */
    struct Digests {
        NativeStateDigest::Digest attacks;
        NativeStateDigest::Digest arsenal;
        NativeStateDigest::Digest effects;
        bool operator==(const Digests&) const = default;
    };
    /** Their digests now, over what they borrow from this scene registered under keys every
     * load of it shares, so that two fixtures can be compared as well. */
    Digests digests() const {
        game::NativeStateResources resources;
        REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
        if (weapons.loaded()) {
            REQUIRE(resources.registerArchive(weapons, "weapons"));
        }
        REQUIRE(resources.registerLayout(world.layout(), "stage"));
        REQUIRE(resources.registerCollision(world.collision(), "stage"));
        const auto attackDigest = liveDigest(attacks, resources.resources());
        const auto arsenalDigest = liveDigest(arsenal, resources);
        return {attackDigest, arsenalDigest, liveDigest(effects, resources)};
    }
};
} // namespace gdl::test
