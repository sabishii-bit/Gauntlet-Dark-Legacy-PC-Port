#include <filesystem>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/io/File.h"
#include "engine/world/TreeModel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

/** A figure of one lit triangle (texture 0) at node offset (1, 0, 0) and a translucent
 * banner (texture 1) two units up. */
std::filesystem::path sampleFigure(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/000_BODY.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 1 0\n"
                  "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeTextFile(dir / "models/001_BANNER.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex1\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects": [
  {"index": 0, "name": "BODY", "file": "models/000_BODY.obj", "meshTriangles": 1},
  {"index": 1, "name": "BANNER", "file": "models/001_BANNER.obj", "meshTriangles": 1}]})");
    writeFile(dir / "textures/000_SKIN.png", test::kTinyPng);
    writeFile(dir / "textures/001_CLOTH.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"defs": [], "bitmaps": [
  {"index": 0, "name": "SKIN", "file": "textures/000_SKIN.png", "width": 2, "height": 2, "flags": 0},
  {"index": 1, "name": "CLOTH", "file": "textures/001_CLOTH.png", "width": 2, "height": 2, "flags": 128}]})");
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "FIGURE", "prefix": "", "sequences": [], "nodes": [
    {"name": "ROOT", "object": "", "type": 0, "flags": 1, "objectFlags": 0, "parent": -1,
     "position": [1.0, 0.0, 0.0]},
    {"name": "BODY", "object": "BODY", "type": 0, "flags": 0, "objectFlags": 0, "parent": 0,
     "position": [0.0, 0.0, 0.0]},
    {"name": "BANNER", "object": "BANNER", "type": 0, "flags": 0, "objectFlags": 0, "parent": 1,
     "position": [0.0, 2.0, 0.0]}]}]})");
    return dir;
}

TEST_CASE("a tree model stands its meshes in the world, lit, opaque parts first",
          "[world][model]") {
    const auto dir = sampleFigure("tree-model");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE_FALSE(figure.bound());
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    REQUIRE(figure.nodeCount() == 2);
    REQUIRE(figure.minBounds() == Vec3{1.0f, 0.0f, 0.0f});
    REQUIRE(figure.maxBounds() == Vec3{2.0f, 3.0f, 0.0f});

    const Mat4 clip = glm::scale(Mat4{1.0f}, Vec3{0.5f, 0.5f, 0.5f});
    const Mat4 model = glm::translate(Mat4{1.0f}, Vec3{5.0f, 0.0f, -2.0f});
    figure.draw(device, clip, model);
    REQUIRE(device.draws.size() == 2);
    // The body (opaque) draws before the banner (translucent), each placed by the model
    // matrix on the CPU and handed the clip transform.
    const ImmediateVertex& corner = device.draws[0].vertices[0];
    REQUIRE(corner.position == Vec3{6.0f, 0.0f, -2.0f});
    REQUIRE(corner.color.r > 200); // the body's normal faces the light
    REQUIRE(device.draws[0].transform == clip);
    REQUIRE(device.draws[1].vertices[0].position == Vec3{6.0f, 2.0f, -2.0f});
    REQUIRE(device.draws[1].texture != device.draws[0].texture);
}

TEST_CASE("a faded tree model blends every part, and draws nothing once unseen",
          "[world][model]") {
    const auto dir = sampleFigure("tree-model-fade");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, WorldLighting{}, {}, nullptr, 0.5f);
    REQUIRE(device.draws.size() == 2);
    for (const auto& draw : device.draws) {
        REQUIRE(draw.state.blend == BlendMode::Alpha);
        REQUIRE_FALSE(draw.state.depthWrite);
        REQUIRE(draw.vertices[0].color.a >= 120); // half the shade's alpha
        REQUIRE(draw.vertices[0].color.a <= 135);
    }
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, WorldLighting{}, {}, nullptr, 0.0f);
    REQUIRE(device.draws.empty());
}

TEST_CASE("a tree model follows a pose's node matrices", "[world][model]") {
    const auto dir = sampleFigure("tree-model-posed");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    const Mat4 clip{1.0f};
    const Mat4 model = glm::translate(Mat4{1.0f}, Vec3{5.0f, 0.0f, -2.0f});
    std::vector<Mat4> transforms(3, Mat4{1.0f});
    transforms[1] = glm::translate(Mat4{1.0f}, Vec3{0.0f, 10.0f, 0.0f});
    transforms[2] = glm::translate(Mat4{1.0f}, Vec3{3.0f, 0.0f, 0.0f});
    figure.draw(device, clip, model, WorldLighting{}, transforms);
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[0].vertices[0].position == Vec3{5.0f, 10.0f, -2.0f});
    REQUIRE(device.draws[1].vertices[0].position == Vec3{8.0f, 0.0f, -2.0f});
    // Nodes past the end of the list keep their rest offset.
    figure.draw(device, clip, model, WorldLighting{}, std::span<const Mat4>(transforms).first(2));
    REQUIRE(device.draws.size() == 4);
    REQUIRE(device.draws[2].vertices[0].position == Vec3{5.0f, 10.0f, -2.0f});
    REQUIRE(device.draws[3].vertices[0].position == Vec3{6.0f, 2.0f, -2.0f});
}

TEST_CASE("a tree model adds glowing nodes onto the frame without writing depth",
          "[world][model]") {
    const auto dir = sampleFigure("tree-model-glow");
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "FIGURE", "prefix": "", "sequences": [], "nodes": [
    {"name": "BODY", "object": "BODY", "type": 0, "flags": 0, "objectFlags": 0, "parent": -1,
     "position": [0.0, 0.0, 0.0]},
    {"name": "GLOW", "object": "BODY", "type": 0, "flags": 0, "objectFlags": 8388736,
     "parent": 0, "position": [0.0, 2.0, 0.0]}]}]})");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 2);
    // The body first, plainly; the glow after it, added, cut out and leaving depth alone.
    REQUIRE(device.draws[0].state.blend == BlendMode::Alpha);
    REQUIRE(device.draws[0].state.depthWrite);
    REQUIRE(device.draws[0].state.alphaTest == 0.0f);
    REQUIRE(device.draws[1].state.blend == BlendMode::Additive);
    REQUIRE_FALSE(device.draws[1].state.depthWrite);
    REQUIRE(device.draws[1].state.alphaTest == DrawState::kTranslucentAlphaTest);
    REQUIRE(device.draws[1].vertices[0].position == Vec3{0.0f, 2.0f, 0.0f});
}

TEST_CASE("nodes flagged to face the camera turn its way", "[world][model]") {
    const auto dir = sampleFigure("tree-model-facing");
    // The body turns about the vertical to the camera (mode 1); the banner takes the camera's
    // whole frame (mode 4).
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "FIGURE", "prefix": "", "sequences": [], "nodes": [
    {"name": "BODY", "object": "BODY", "type": 0, "flags": 0, "objectFlags": 16777216,
     "parent": -1, "position": [0.0, 0.0, 0.0]},
    {"name": "BANNER", "object": "BANNER", "type": 0, "flags": 0, "objectFlags": 67108864,
     "parent": 0, "position": [0.0, 2.0, 0.0]}]}]})");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    // Without a camera nothing turns.
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws[0].vertices[1].position == Vec3{1.0f, 0.0f, 0.0f});
    device.draws.clear();
    // A camera off to +x: the body's z axis swings to point at it, so its x edge lies along -z.
    CameraFrame camera;
    camera.position = Vec3{10.0f, 0.0f, 0.0f};
    camera.right = Vec3{0.0f, 0.0f, -1.0f};
    camera.forward = Vec3{-1.0f, 0.0f, 0.0f};
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, WorldLighting{}, {}, &camera);
    REQUIRE(device.draws.size() == 2);
    const Vec3 edge = device.draws[0].vertices[1].position;
    REQUIRE(edge.x == Approx(0.0f).margin(1e-5f));
    REQUIRE(edge.z == Approx(-1.0f).margin(1e-5f));
    // The banner's x edge follows minus the camera's right, and it keeps its height.
    const Vec3 corner = device.draws[1].vertices[1].position;
    REQUIRE(corner == Vec3{0.0f, 2.0f, 1.0f});
    REQUIRE(device.draws[1].vertices[2].position == Vec3{0.0f, 3.0f, 0.0f});
}

TEST_CASE("a tree model refuses a figure with a missing mesh", "[world][model]") {
    const auto dir = sampleFigure("tree-model-missing");
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "FIGURE", "prefix": "", "sequences": [], "nodes": [
    {"name": "GHOST", "object": "GHOST", "type": 0, "flags": 0, "objectFlags": 0, "parent": -1,
     "position": [0.0, 0.0, 0.0]}]}]})");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE_FALSE(figure.bind(trees.tree(0), models, textures, device));
    REQUIRE_FALSE(figure.bound());
}

} // namespace
