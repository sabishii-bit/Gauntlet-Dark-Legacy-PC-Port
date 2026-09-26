#pragma once

#include <filesystem>
#include <string>

#include "engine/io/File.h"

#include "TestSupport.h"

namespace gdl::test {
inline std::filesystem::path deathArchive() {
    const auto root = scratchDirectory("death-enemy");
    const auto dir = root / "MONSTERS/DEATH";
    std::filesystem::create_directories(dir);
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    std::string trees = R"({"trees":[)";
    for (const auto* name :
         {"DEATH1", "DEATH2", "DEATH_ARC", "DEATH_EXP", "DEATHSTATUE1", "DEATHSTATUE2"}) {
        if (trees.back() != '[') {
            trees += ',';
        }
        trees += R"({"name":")" + std::string(name) + R"(",
            "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"READY","frames":13,"rate":30}]})";
    }
    writeTextFile(dir / "animations.json", trees + "]}");
    return root;
}
} // namespace gdl::test
