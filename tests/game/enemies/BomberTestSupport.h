#pragma once

#include <filesystem>
#include <string>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/EnemyKinds.h"

namespace gdl::test {
/** A scratch root whose grunt can be a suicide bomber: its stance, the wind-up into its run and
 * the run, so that a bomber sees a player, burns its fuse and cries out as its run begins. */
inline std::filesystem::path bomberArchive() {
    const auto root = scratchDirectory("suicide-bomber");
    const std::string prefix{game::enemyKind(game::kGruntKind).prefix};
    const auto dir = root / "MONSTERS" / prefix;
    std::filesystem::create_directories(dir);
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    convertModelFixture(dir);
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":")" + prefix + R"(1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":2,"rate":30},
                     {"name":"READYTOWALK","frames":4,"rate":30},
                     {"name":"RUN","frames":10,"rate":30}]}]})");
    return root;
}
} // namespace gdl::test
