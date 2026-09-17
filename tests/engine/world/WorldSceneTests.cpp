#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"

namespace {

using namespace gdl;

/** A level with one group holding a lit wall (texture 0) and a translucent window (texture 1);
 * a third object has no mesh. */
std::filesystem::path sampleLevel(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/000_WALL.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 1 0\n"
                  "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeTextFile(dir / "models/001_WINDOW.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 -1 0\nusemtl tex1\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({
  "source": "/sample/",
  "objects": [
    {"index": 0, "name": "WALL", "file": "models/000_WALL.obj", "meshTriangles": 1},
    {"index": 1, "name": "WINDOW", "file": "models/001_WINDOW.obj", "meshTriangles": 1}
  ]
})");
    writeFile(dir / "textures/000_STONE.png", test::kTinyPng);
    writeFile(dir / "textures/001_GLASS.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({
  "source": "/sample/",
  "defs": [],
  "bitmaps": [
    {"index": 0, "name": "STONE", "file": "textures/000_STONE.png", "width": 2, "height": 2,
     "format": 50, "flags": 0, "halfResolution": false, "frames": 0},
    {"index": 1, "name": "GLASS", "file": "textures/001_GLASS.png", "width": 2, "height": 2,
     "format": 50, "flags": 128, "halfResolution": false, "frames": 0}
  ]
})");
    writeTextFile(dir / "world.json", R"({
  "objects": [
    {"name": "GROUP", "position": [10, 0, 0], "next": -1, "child": 1},
    {"name": "WINDOW", "position": [0, 5, 0], "next": 2, "child": -1},
    {"name": "WALL", "position": [1, 0, 0], "next": 3, "child": -1},
    {"name": "NOTHING", "position": [0, 0, 9], "next": -1, "child": -1}
  ],
  "locators": []
})");
    return dir;
}

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;

    explicit Fixture(std::string_view name) {
        const auto dir = sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
    }
};

TEST_CASE("a world scene places every object that has a mesh", "[world][scene]") {
    Fixture f("world-scene");
    REQUIRE(f.scene.build(f.layout, f.models, f.textures, f.device));
    REQUIRE(f.scene.built());
    REQUIRE(f.scene.placedCount() == 2);
    REQUIRE(f.scene.batchCount() == 2);
    REQUIRE(f.scene.triangleCount() == 2);

    f.scene.draw(f.device, Mat4{1.0f});
    REQUIRE(f.device.draws.size() == 2);
    // The opaque wall draws first, offset by its parent; the translucent window follows.
    const auto& wall = f.device.draws[0];
    REQUIRE(wall.vertices.size() == 3);
    REQUIRE(wall.vertices[0].position == Vec3{11.0f, 0.0f, 0.0f});
    REQUIRE(wall.vertices[1].position == Vec3{12.0f, 0.0f, 0.0f});
    const auto& window = f.device.draws[1];
    REQUIRE(window.vertices[0].position == Vec3{10.0f, 5.0f, 0.0f});
    // Lit from above, the upward-facing wall is brighter than the downward-facing window.
    REQUIRE(wall.vertices[0].color.r > window.vertices[0].color.r);
    REQUIRE(window.vertices[0].color.r > 0);

    f.scene.clear();
    REQUIRE_FALSE(f.scene.built());
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1.0f});
    REQUIRE(f.device.draws.empty());
}

TEST_CASE("a layout whose objects have no meshes builds nothing", "[world][scene]") {
    Fixture f("world-scene-empty");
    const auto dir = test::scratchDirectory("world-scene-empty-layout");
    writeTextFile(dir / "world.json",
                  R"({"objects": [{"name": "GHOST", "position": [0, 0, 0]}]})");
    WorldLayout ghosts;
    REQUIRE(ghosts.load(dir));
    REQUIRE_FALSE(f.scene.build(ghosts, f.models, f.textures, f.device));
    REQUIRE(f.scene.placedCount() == 0);
}

} // namespace
