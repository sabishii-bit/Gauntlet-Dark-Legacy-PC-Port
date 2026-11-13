#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/characters/CharacterSave.h"
#include "game/effects/EffectTrees.h"
#include "game/party/PartyTeleport.h"
#include "game/party/PlayerRuntime.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A WEAPONS stand-in holding nothing but the teleport's STARTFX tree. */
std::filesystem::path teleportArchive() {
    const auto root = test::scratchDirectory("party-teleport");
    writeTextFile(root / "mesh.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 1 0\n"
                                     "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeFile(root / "skin.png", test::kTinyPng);
    writeTextFile(root / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"mesh.obj","meshTriangles":1}]})");
    writeTextFile(root / "textures.json", R"({"defs":[],"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2,"flags":0}]})");
    test::convertModelFixture(root);
    writeTextFile(root / "animations.json",
                  R"({"textureAnimations":[],"trees":[{"name":"STARTFX","nodes":[
        {"name":"ROOT","object":"BODY","position":[0,0,0]}],"sequences":[
        {"name":"START","frames":8,"frameRate":30,"repeats":false,
         "tracks":[{"node":0,"flags":16,"frames":[0,4],"values":[0,4]}]}]}]})");
    return root;
}

/** Four members: two standing, one fallen to the tower and one gone from the game. */
std::array<PlayerRuntime, 4> partyOfFour() {
    std::array<PlayerRuntime, 4> party;
    for (usize i = 0; i < party.size(); ++i) {
        party[i].actor.spawn(static_cast<s32>(i), CharacterSave{}, nullptr,
                             Vec3{static_cast<f32>(i) * 3.0f, 0.0f, -2.0f}, 0.0f);
    }
    party[1].life = PlayerLife::InTower;
    party[3].departed = true;
    return party;
}

} // namespace

TEST_CASE("the teleport sparkles once at the feet of each member still standing",
          "[game][party][teleport]") {
    ItemArchive weapons;
    REQUIRE(weapons.load(teleportArchive()));
    REQUIRE(weapons.trees.find(PartyTeleport::kEffect).has_value());
    test::FakeRenderDevice device;
    EffectTrees effects;
    const auto party = partyOfFour();
    PartyTeleport::sparkle(device, effects, weapons, party);
    REQUIRE(effects.count() == 2);
    std::vector<Vec3> where;
    for (usize i = 0; i < effects.count(); ++i) {
        CHECK(effects.effect(i).name == PartyTeleport::kEffect);
        where.push_back(effects.effect(i).position);
    }
    CHECK(where == std::vector{party[0].actor.position(), party[2].actor.position()});
}

TEST_CASE("the teleport shows nothing, and fails nothing, without the weapons archive",
          "[game][party][teleport]") {
    ItemArchive weapons;
    REQUIRE_FALSE(weapons.loaded());
    test::FakeRenderDevice device;
    EffectTrees effects;
    const auto party = partyOfFour();
    PartyTeleport::sparkle(device, effects, weapons, party);
    CHECK(effects.count() == 0);
}
