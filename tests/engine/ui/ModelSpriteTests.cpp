#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/ModelSprite.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

/** A tree of one triangle mesh offset by (1, 0, 0), textured with a 2x2 PNG. */
std::filesystem::path sampleScene(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/000_ICON_ARROWFR.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 0 1\n"
                  "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeTextFile(dir / "objects.json", R"({"objects": [
  {"index": 0, "name": "ICON_ARROWFR", "file": "models/000_ICON_ARROWFR.obj", "meshTriangles": 1}]})");
    writeFile(dir / "textures/000_T.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"defs": [], "bitmaps": [
  {"index": 0, "name": "T", "file": "textures/000_T.png", "width": 2, "height": 2, "flags": 0}]})");
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "ICON_ARROW", "prefix": "ICON_ARROW", "sequences": [], "nodes": [
    {"name": "ROOT", "object": "", "type": 1, "flags": 1, "objectFlags": 0, "parent": -1,
     "position": [1.0, 0.0, 0.0]},
    {"name": "FR", "object": "ICON_ARROWFR", "type": 1, "flags": 0, "objectFlags": 0,
     "parent": 0, "position": [0.0, 0.0, 0.0]}]}]})");
    return dir;
}

TEST_CASE("a model sprite draws its meshes lit and placed on the canvas", "[ui][model]") {
    const auto dir = sampleScene("model-sprite");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    ModelSprite sprite;
    REQUIRE_FALSE(sprite.bound());
    REQUIRE(sprite.bind(trees.tree(0), models, textures, device));
    REQUIRE(sprite.nodeCount() == 1);

    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    sprite.draw(canvas, Vec2{100.0f, 50.0f}, 10.0f, 0.0f);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws[0].vertices.size() == 3);
    const ImmediateVertex& origin = device.draws[0].vertices[0];
    REQUIRE(origin.position == Vec3{1.0f, 0.0f, 0.0f}); // the node offset, before the transform
    REQUIRE(origin.color.r > 200);                      // facing the light
    const Vec4 placed = device.draws[0].transform * Vec4{origin.position, 1.0f};
    REQUIRE(placed.x == Approx(110.0f));
    REQUIRE(placed.y == Approx(50.0f));
    REQUIRE(placed.z == Approx(ModelSprite::kDepth));
    const Vec4 up = device.draws[0].transform * Vec4{1.0f, 1.0f, 0.0f, 1.0f};
    REQUIRE(up.y == Approx(40.0f)); // model y up becomes screen y up
}

TEST_CASE("binding fails when a mesh or texture is missing", "[ui][model]") {
    const auto dir = sampleScene("model-sprite-missing");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "X", "prefix": "", "sequences": [], "nodes": [
    {"name": "A", "object": "NOPE", "type": 1, "flags": 0, "objectFlags": 0, "parent": -1,
     "position": [0, 0, 0]}]}]})");
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    ModelSprite sprite;
    REQUIRE_FALSE(sprite.bind(trees.tree(0), models, textures, device));
    REQUIRE_FALSE(sprite.bound());
}

} // namespace
