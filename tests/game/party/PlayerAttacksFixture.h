#pragma once

#include <array>
#include <filesystem>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/world/AmbientDimmer.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/audio/LevelSoundscape.h"
#include "game/camera/CameraShake.h"
#include "game/characters/ClassData.h"
#include "game/config/MultiplayerMode.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/LevelOpponents.h"
#include "game/level/LevelFixtures.h"
#include "game/level/LevelWorld.h"
#include "game/level/ScriptedFixtures.h"
#include "game/party/PlayerArsenal.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/PlayerRuntime.h"
#include "game/party/ScriptedAttacks.h"

namespace gdl::test {
/** PlayerAttacks bound over an empty level with its arsenal, for one party member (player 3
 * at the origin), the fixtures' and attacks' hosts scripted; each melee hit reported is kept
 * in `meleeHits`. */
struct PlayerAttacksFixture {
    FakeRenderDevice device;
    game::ClassDataSet classes;
    game::LevelWorld world;
    ItemArchive weapons;
    game::EffectTrees effects;
    game::LevelSoundscape audio;
    AmbientDimmer dimmer;
    game::CameraShake shake;
    game::PlayerArsenal arsenal;
    game::LevelOpponents opponents;
    game::LevelFixtures fixtures;
    game::PlayerAttacks attacks;
    game::MultiplayerMode mode = game::MultiplayerMode::Normal;
    std::array<game::PlayerRuntime, 1> players;
    std::vector<s32> meleeHits;
    FixtureScript fixtureEvents;
    ScriptedFixtures fixtureHost{fixtureEvents};
    AttackScript attackEvents;
    ScriptedAttacks attackHost{attackEvents};
    game::PlayerAttacks::Targets targets{opponents, fixtures, fixtureHost, attackHost};
    PlayerAttacksFixture() {
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
                      nullptr,
                      &mode});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer,
                      &shake, &mode});
        players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
        attackEvents.meleeHit = [this](s32 player) { meleeHits.push_back(player); };
    }
};

/** A scratch root of synthetic archives: the warrior's yellow costume, its ANIM and SFXYEL
 * archives and a PDATA whose lesser turbo is a burst and greater a wave, and the grunt's. */
inline std::filesystem::path turboAssets() {
    const auto root = scratchDirectory("turbo-contacts");
    for (const auto* name : {"PLAYERS/WAR/YEL", "PLAYERS/WAR/SFXYEL", "MONSTERS/GRU"}) {
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
      {"name":"ATTPWRC","frames":60,"rate":30}]}]})");
    writeTextFile(root / "PLAYERS/WAR/SFXYEL/animations.json", R"({"trees":[{"name":"BURST",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"ACTIVE","frames":60,"rate":30}]}]})");
    std::filesystem::create_directories(root / "pdata");
    writeTextFile(root / "pdata/WAR.json", R"({"height":6,"width":2,
      "fight":[200,600],"speed":[200,600],"armor":[200,600],"magic":[200,600],
      "moves":{"turboB":0,"turboC1":1},"moveEffects":[{"tree":"BURST"}],"moveStrikes":[
      {"type":4,"startFrame":1,"radius":12,"arc":-1,"delay":0.1,"amount":50,"effect":0},
      {"type":2,"startFrame":1,"hitRadius":10,"arc":-1,"offset":[0,9,2],
       "speedMin":30,"speedMax":30,"maxTime":6,"amount":70,"flags":64,
       "damageType":1048576}]})");
    return root;
}
} // namespace gdl::test
