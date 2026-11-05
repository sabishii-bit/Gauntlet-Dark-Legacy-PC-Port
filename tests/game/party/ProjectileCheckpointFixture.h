#pragma once

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/party/PlayerMissiles.h"

namespace gdl::test {
inline std::filesystem::path projectileCheckpointArchive() {
    const auto root = scratchDirectory("projectile-pool-checkpoint");
    writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
    writeFile(root / "white.png", kTinyPng);
    writeTextFile(root / "textures.json", R"({"bitmaps":[
        {"name":"WHITE","file":"white.png","width":2,"height":2}]})");
    writeTextFile(root / "animations.json", R"({
      "particles":[{"enables":16928,"texture":"WHITE","particleLife":[1,0],
        "rate":[30,30,30,30],"flags":128,"flagMask":128}],
      "trees":[{"name":"FLIGHT","nodes":[
        {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
        {"name":"EMITTER","parent":0,"position":[0,1,0],"particle":0}],
        "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30,
          "tracks":[{"node":0,"flags":32,"frames":[0,8],"values":[0,2]}]}]}]})");
    convertModelFixture(root);
    return root;
}

/** A projectile archive whose artwork the prototypes bind before any pool counts uploads.
 * They outlive every pool that borrows the archive. */
struct ProjectileCheckpointFixture {
    FakeRenderDevice device;
    ItemArchive archive;
    TextureAnimator textures;
    game::EffectTrees prototypes;
    ProjectileCheckpointFixture() {
        REQUIRE(archive.load(projectileCheckpointArchive()));
        game::EffectTrees::Setting setting;
        setting.persistent = true;
        REQUIRE(prototypes.startSet(device, archive, "FLIGHT", {}, setting) != 0);
        textures.bind(archive.trees.textureAnimations(), archive.textures, device);
    }
    game::MissileLaunch launch() {
        game::MissileLaunch result;
        result.spec = &game::MissileSpec::of(0);
        result.archive = &archive;
        result.tree = "FLIGHT";
        result.position = {0, 8, 0};
        result.velocity = Vec3{0, 0, 20};
        result.release = game::MissileRelease{0, 72, false};
        return result;
    }
};
} // namespace gdl::test
