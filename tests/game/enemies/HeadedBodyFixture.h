#pragma once
#include <filesystem>
#include <string>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"

namespace gdl::test {
/** A synthetic boss with two heads of its own: the body's HEAD looks about, each head is a
 * branch (HEAD_L, HEAD_R) with a look node at its tip, a spit of its own and a pattern the
 * body's drives, so head scheduling, gaze and death can be checked without game data. */
inline std::filesystem::path headedBodyAssets() {
    const auto root = scratchDirectory("headed-body");
    std::filesystem::create_directories(root / "critter");
    const auto archive = root / "MONSTERS" / "CHIMERA";
    std::filesystem::create_directories(archive / "models");
    std::filesystem::create_directories(archive / "textures");
    writeTextFile(archive / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"models/body.obj","meshTriangles":1}]})");
    writeFile(archive / "textures/skin.png", kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]},
               {"name":"NECK","object":"BODY","parent":0,"position":[0,5,0]},
               {"name":"HEAD","object":"BODY","parent":1,"position":[0,1,0]},
               {"name":"HEAD_L","object":"BODY","parent":0,"position":[2,5,0]},
               {"name":"HEAD_L_TIP","object":"BODY","parent":3,"position":[0,1,0]},
               {"name":"HEAD_R","object":"BODY","parent":0,"position":[-2,5,0]},
               {"name":"HEAD_R_TIP","object":"BODY","parent":5,"position":[0,1,0]}],
      "sequences":[{"name":"READY","frames":30},{"name":"START","frames":6},
                   {"name":"DEATH","frames":6},{"name":"SPIT","frames":6},
                   {"name":"HIT","frames":6}]}]})");
    const std::string head = R"("maxHealth":100,"radius":2,"expValue":50,"parentIndex":0,
       "moveCount":4,"patternCount":1,"lookYawRate0":0.5,"lookPitchRate0":0.5)";
    writeTextFile(root / "critter/CHIMERA.json", R"({
      "descriptors":[{"prefix":"BODY","name":"CHIMERA","type":4}],
      "types":[{"moveCount":6,"patternCount":1,"maxHealth":1000,"radius":3,"expValue":500,
                "childIndex":1,"lookNode0":"HEAD","lookYawRate0":0.3,"lookPitchRate0":0.2,
                "lookPitchBias0":0.1},
               {"rootNode":"HEAD_L","lookNode0":"HEAD_L_TIP","moveIndex":6,"patternIndex":1,
                "childIndex":2,)" + head + R"(},
               {"rootNode":"HEAD_R","lookNode0":"HEAD_R_TIP","moveIndex":10,"patternIndex":2,
                "childIndex":-1,)" + head + R"(}],
      "moves":[{"name":"START","anim":"START","type":16,"priority":4095,"interrupt":0},
               {"name":"READY","anim":"READY","type":32,"priority":512,"interrupt":20},
               {"name":"SYNC","anim":"READY","type":1,"priority":512,"interrupt":20},
               {"name":"DEATH","anim":"DEATH","type":17,"priority":4095,"interrupt":0,"hold":1},
               {"name":"STARE","anim":"READY","type":128,"flags":1,"priority":544,
                "interrupt":20,"cooldown":100,"target":{"maxDistance":5}},
               {"name":"READYP","anim":"READY","type":32,"flags":4,"priority":512,
                "interrupt":20},
               {"name":"READY","anim":"READY","type":32,"priority":512,"interrupt":20},
               {"name":"DEATH","anim":"DEATH","type":17,"priority":4095,"interrupt":0,"hold":1},
               {"name":"SPIT","anim":"SPIT","type":128,"priority":544,"interrupt":20,
                "sfx":0,"sfxFrame":0,"cooldown":100,"target":{"maxDistance":200}},
               {"name":"HIT","anim":"HIT","type":64,"priority":576,"interrupt":20,"cooldown":15},
               {"name":"READY","anim":"READY","type":32,"priority":512,"interrupt":20},
               {"name":"DEATH","anim":"DEATH","type":17,"priority":4095,"interrupt":0,"hold":1},
               {"name":"SPIT","anim":"SPIT","type":128,"priority":544,"interrupt":20,
                "sfx":0,"sfxFrame":0,"cooldown":100,"target":{"maxDistance":200}},
               {"name":"HIT","anim":"HIT","type":64,"priority":576,"interrupt":20,"cooldown":15}],
      "patterns":[{"flags":0,"cooldown":100,"moves":[5,5,-1,-1,-1,-1,-1,-1],
                   "target":{"minDistance":30}},
                  {"flags":4096,"cooldown":0,"moves":[2,2,-1,-1,-1,-1,-1,-1]},
                  {"flags":4096,"cooldown":0,"moves":[2,2,-1,-1,-1,-1,-1,-1]}],
      "sounds":[{"name":"SPITFX"}]})");
    return root;
}
} // namespace gdl::test
