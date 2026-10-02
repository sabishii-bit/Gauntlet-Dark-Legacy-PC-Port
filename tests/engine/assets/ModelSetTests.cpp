#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"

namespace {

using namespace gdl;

std::filesystem::path sampleSet(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "models");
    writeTextFile(dir / "models/000_TRI.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex3\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({
  "source": "/sample/",
  "objects": [
    {"index": 0, "name": "TRI", "file": "models/000_TRI.obj", "meshTriangles": 1},
    {"index": 1, "name": "EMPTY"}
  ]
})");
    test::convertModelFixture(dir);
    return dir;
}

TEST_CASE("a model set finds objects by name and loads their meshes", "[model]") {
    ModelSet set;
    REQUIRE(set.load(sampleSet("model-set")));
    REQUIRE(set.size() == 2);
    REQUIRE(set.find("tri") == 0U);
    REQUIRE(set.entry(0).triangles == 1);
    REQUIRE_FALSE(set.find("nothing").has_value());
    const Mesh& mesh = set.mesh(0);
    REQUIRE(mesh.triangleCount() == 1);
    REQUIRE(mesh.parts[0].texture == 3);
    REQUIRE(&set.mesh(0) == &mesh);
    REQUIRE(set.mesh(1).vertices.empty());
}

TEST_CASE("missing native models fail even when inspection exports exist", "[model]") {
    ModelSet set;
    REQUIRE_FALSE(set.load(test::scratchDirectory("model-set-empty")));
    REQUIRE_FALSE(set.loaded());
    const auto dir = test::scratchDirectory("model-set-export-only");
    writeTextFile(dir / "objects.json", R"({"objects":[{"name":"STALE"}]})");
    REQUIRE_FALSE(set.load(dir));
    REQUIRE_FALSE(set.find("STALE"));
}

TEST_CASE("the native powerups hold the arrow meshes", "[assets][model]") {
    const auto dir = test::assetOrSkip("POWERUPS/objects.ngc").parent_path();
    ModelSet set;
    REQUIRE(set.load(dir));
    const auto index = set.find("ICON_ARROWFR#0");
    REQUIRE(index.has_value());
    REQUIRE(set.mesh(*index).triangleCount() == 61);
}

TEST_CASE("native fixture construction preserves geometry bindings and texture metadata",
          "[model][native-fixture]") {
    const auto dir = sampleSet("native-model-fixture");
    writeTextFile(dir / "models/000_TRI.obj",
                  "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\n"
                  "vt 0 1\nvt 1 1\nvt 0 0\nvl 0.25 0.5\nvl 0.75 0.5\nvl 0.25 0.25\n"
                  "vn 0 0 1\nusemtl tex0_lm1\nf 1/1/1 2/2/1 3/3/1\n"
                  "usemtl tex1\nf 1/1/1 3/3/1 2/2/1\n");
    writeFile(dir / "tiny.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"name":"COLOUR","width":8,"height":4,"file":"tiny.png","clampU":true,
         "halfResolution":true,"frames":2},
        {"name":"EMPTY","width":1,"height":1,"file":"absent.png","flags":256}],
        "defs":[{"name":"ALIAS","index":0}]})");
    test::convertModelFixtures(dir);
    ModelSet models;
    REQUIRE(models.load(dir));
    const auto& mesh = models.mesh(0);
    REQUIRE(mesh.triangleCount() == 2);
    REQUIRE(mesh.parts.size() == 2);
    REQUIRE(mesh.parts[0].texture == 0);
    REQUIRE(mesh.parts[0].lightmap == 1);
    REQUIRE(mesh.parts[1].texture == 1);
    REQUIRE(mesh.vertices[1].position == Vec3{1, 0, 0});
    REQUIRE(mesh.vertices[1].normal == Vec3{0, 0, 1});
    REQUIRE(mesh.vertices[1].uv == Vec2{1, 0});
    REQUIRE(mesh.vertices[1].lightmapUv == Vec2{0.75f, 0.5f});
    REQUIRE(mesh.prelit);
    REQUIRE(mesh.vertices[0].color == Color::rgba(248, 0, 0));
    REQUIRE(mesh.parts[0].indices == std::vector<u32>{0, 1, 2});
    TextureSet textures;
    REQUIRE(textures.load(dir));
    REQUIRE(textures.find("ALIAS") == 0);
    REQUIRE(textures.entry(0).width == 8);
    REQUIRE(textures.entry(0).height == 4);
    REQUIRE(textures.entry(0).clampU);
    REQUIRE(textures.entry(0).halfResolution);
    REQUIRE(textures.entry(0).frames == 2);
    REQUIRE(textures.image(0).pixel(0, 0) == Color::rgba(255, 0, 0));
    REQUIRE(textures.image(0).pixel(7, 0) == Color::rgba(0, 255, 0));
    REQUIRE(textures.image(0).pixel(0, 3) == Color::rgba(0, 0, 255));
    REQUIRE(textures.image(0).pixel(7, 3).a == 0);
    REQUIRE(textures.image(1).pixels == std::vector<u8>(4, 0));
}

TEST_CASE("native fixtures reject names that do not fit the archive", "[model][native-fixture]") {
    const auto dir = test::scratchDirectory("native-model-long-name");
    writeTextFile(dir / "objects.json", R"({"objects":[{"name":"LONGER_THAN_SIXTEEN"}]})");
    REQUIRE_THROWS_AS(test::convertModelFixture(dir), FormatError);
    REQUIRE_FALSE(std::filesystem::exists(dir / "objects.ngc"));
}

} // namespace
