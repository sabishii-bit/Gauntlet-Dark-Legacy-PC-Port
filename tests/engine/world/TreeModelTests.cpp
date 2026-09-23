#include <filesystem>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/TextureAnimator.h"
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

TEST_CASE("alternate skin preserves base coverage and the original opaque or translucent material",
          "[world][model]") {
    const auto dir = sampleFigure("tree-model-masked");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    test::FakeTexture ice{1, 1};
    figure.setMaskedTexture(&ice);
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[0].texture == &textures.texture(device, 0));
    REQUIRE(device.draws[1].texture == &textures.texture(device, 1));
    REQUIRE(device.draws[0].state.maskedTexture == &ice);
    REQUIRE(device.draws[1].state.maskedTexture == &ice);
    REQUIRE(device.draws[0].state.blend == BlendMode::Opaque);
    REQUIRE(device.draws[0].state.depthWrite);
    REQUIRE(device.draws[1].state.blend == BlendMode::Alpha);
    REQUIRE(device.draws[1].state.alphaTest == DrawState::kTranslucentAlphaTest);
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, {}, nullptr, 0.5f);
    REQUIRE(device.draws[0].state.blend == BlendMode::Alpha);
    REQUIRE_FALSE(device.draws[0].state.depthWrite);
    figure.resetTextures();
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws[0].state.maskedTexture == nullptr);
}

TEST_CASE(
    "effect appearance stays fully lit and tinted without changing depth flags on other models",
    "[world][model]") {
    const auto dir = sampleFigure("tree-model-appearance");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    WorldLighting dark;
    dark.ambient = dark.lightColor = Vec3{0.0f};
    const Color blue = Color::rgba(0, 0, 255);
    figure.setAppearance(true, blue, false);
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, dark);
    REQUIRE(device.draws[0].vertices[0].color == blue);
    REQUIRE_FALSE(device.draws[0].state.depthWrite);
    // A new binding resets instance overrides.
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, dark);
    REQUIRE(device.draws[0].vertices[0].color == Color::black());
    REQUIRE(device.draws[0].state.depthWrite);
}

TEST_CASE("a faded tree model blends every part, and draws nothing once unseen", "[world][model]") {
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

TEST_CASE("an object node shows the mesh of its run that the frame calls for", "[world][model]") {
    const auto dir = sampleFigure("tree-model-frames");
    // The node's run starts at the body and goes on to the banner, from frame one.
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "FLIP", "prefix": "", "sequences": [
     {"name": "ACTIVE", "frames": 6, "frameRate": 30, "repeats": false, "fixesPosition": false,
      "flags": 0, "tracks": []},
     {"name": "IDLE", "frames": 2, "frameRate": 30, "repeats": true, "fixesPosition": false,
      "flags": 0, "tracks": []}],
   "nodes": [
    {"name": "GLOW", "object": "", "type": 2, "flags": 1, "objectFlags": 0, "parent": -1,
     "position": [0.0, 0.0, 0.0],
     "objectFrames": [{"object": "BODY", "start": 1, "frames": 2},
                      {"object": "", "start": 0, "frames": 0}]}]}]})");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel flame;
    REQUIRE(flame.bind(trees.tree(0), models, textures, device));
    REQUIRE(flame.nodeCount() == 1);
    REQUIRE(flame.maxBounds() == Vec3{1.0f, 1.0f, 0.0f}); // both meshes counted
    // Nothing shows until a frame is set, nor before the run or after it.
    flame.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.empty());
    flame.setFrame(0, 0);
    flame.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.empty());
    flame.setFrame(0, 1);
    flame.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws[0].state.blend == BlendMode::Alpha);
    REQUIRE(device.draws[0].state.alphaTest == 0.0f); // the body's texture is solid
    flame.setFrame(0, 2);
    flame.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[1].state.alphaTest > 0.0f); // the banner's is translucent
    REQUIRE(device.draws[1].texture != device.draws[0].texture);
    flame.setFrame(0, 3);
    flame.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 2);
    // A sequence the node has no run for shows nothing either.
    flame.setFrame(1, 0);
    flame.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 2);
    flame.setFrame(7, 0);
    flame.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 2);
}

TEST_CASE("the spawn effect's flame column comes and goes with its frames",
          "[world][model][unpacked]") {
    const std::filesystem::path dir = test::unpackedOrSkip("WEAPONS/animations.json").parent_path();
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    const auto index = trees.find("STARTFX");
    REQUIRE(index.has_value());
    test::FakeRenderDevice device;
    TreeModel effect;
    REQUIRE(effect.bind(trees.tree(*index), models, textures, device));
    REQUIRE(effect.maxBounds().y >= 16.0f); // the column's spikes reach up
    // The floor's six sparkle polygons alone until the frame is set; then the column too, for
    // the run's thirteen frames.
    effect.draw(device, Mat4{1.0f}, Mat4{1.0f});
    const usize floorOnly = device.draws.size();
    REQUIRE(floorOnly == 6);
    effect.setFrame(0, 0);
    effect.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() > 2 * floorOnly);
    device.draws.clear();
    effect.setFrame(0, 12);
    effect.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() > floorOnly);
    device.draws.clear();
    effect.setFrame(0, 13);
    effect.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == floorOnly);
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

TEST_CASE("a tree model shows texture frames and slides coordinates it is given",
          "[world][model]") {
    const auto dir = sampleFigure("tree-model-motion");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    const test::FakeTexture frame{2, 2};
    figure.setTextureOffset(0, Vec2{0.25f, 0.0f}, Vec2{2.0f, 1.0f});
    figure.setTextureFrame(1, &frame);
    REQUIRE(figure.textureOffset(0) == Vec2{0.25f, 0.0f});
    REQUIRE(figure.textureScale(0) == Vec2{2.0f, 1.0f});
    REQUIRE(figure.textureOffset(7) == Vec2{0.0f, 0.0f});
    REQUIRE(figure.textureScale(7) == Vec2{1.0f, 1.0f});
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[0].state.uvOffset == Vec2{0.25f, 0.0f}); // the body, texture 0
    REQUIRE(device.draws[0].state.uvScale == Vec2{2.0f, 1.0f});
    REQUIRE(device.draws[1].texture == &frame); // the banner, texture 1
    REQUIRE(device.draws[1].state.uvOffset == Vec2{0.0f, 0.0f});
    REQUIRE(device.draws[1].state.uvScale == Vec2{1.0f, 1.0f});
    // Slid again without a stretch, the stretch is gone.
    figure.setTextureOffset(0, Vec2{0.5f, 0.0f});
    REQUIRE(figure.textureScale(0) == Vec2{1.0f, 1.0f});
    // Forgotten again, the set's own textures and coordinates come back.
    figure.setTextureFrame(1, nullptr);
    figure.resetTextures();
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws[0].state.uvOffset == Vec2{0.0f, 0.0f});
    REQUIRE(device.draws[0].state.uvScale == Vec2{1.0f, 1.0f});
    REQUIRE(device.draws[1].texture != &frame);
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

TEST_CASE("marker nodes (a figure's DUMMY, a chest's NULL1) are never drawn", "[world][model]") {
    const auto dir = sampleFigure("tree-model-marker");
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "FIGURE", "prefix": "", "sequences": [], "nodes": [
    {"name": "ROOT", "object": "", "type": 0, "flags": 1, "objectFlags": 0, "parent": -1,
     "position": [0.0, 0.0, 0.0]},
    {"name": "BODY", "object": "BODY", "type": 0, "flags": 0, "objectFlags": 0, "parent": 0,
     "position": [0.0, 0.0, 0.0]},
    {"name": "DUMMY", "object": "BANNER", "type": 0, "flags": 0, "objectFlags": 0, "parent": 0,
     "position": [0.0, 0.0, 0.0]},
    {"name": "NULL1", "object": "BANNER", "type": 0, "flags": 0, "objectFlags": 0, "parent": 0,
     "position": [0.0, 0.0, 0.0]}]}]})");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    REQUIRE(figure.nodeCount() == 1);
    REQUIRE(figure.maxBounds() == Vec3{1.0f, 1.0f, 0.0f}); // the body alone
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 1);
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

TEST_CASE("tree texture overrides follow each sequence and reset between shared instances",
          "[world][animation][genie]") {
    const auto dir = sampleFigure("tree-keyed-textures");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    TreeInfo tree = trees.tree(0);
    tree.sequences.resize(3);
    tree.sequences[0].textureAnimationStart = 1;
    tree.sequences[0].textureAnimationCount = 1;
    tree.sequences[1].textureAnimationStart = 2;
    tree.sequences[1].textureAnimationCount = 1;
    std::vector<TextureAnimationInfo> animations(3);
    animations[0].texture = 0;
    animations[0].source = 0;
    animations[0].frames = 2;
    animations[0].rate = 1;
    animations[1] = animations[0];
    animations[1].source = 1;
    animations[1].frames = 1; // idle sequence pins its own frame over the global cycle
    animations[2] = animations[0];
    animations[2].rate = 2;
    test::FakeRenderDevice device;
    TreeModel model;
    REQUIRE(model.bind(tree, models, textures, device));
    TextureAnimator animator;
    animator.bind(animations, textures, device);
    const auto drawn = [&](u32 sequence, s32 frame) {
        device.draws.clear();
        animator.apply(model, tree, sequence, frame);
        model.draw(device, Mat4{1}, Mat4{1});
        REQUIRE(device.draws.size() == 2);
        return device.draws.front().texture;
    };
    REQUIRE(drawn(0, 20) == &textures.texture(device, 1));
    REQUIRE(drawn(1, 0) == &textures.texture(device, 0));
    REQUIRE(drawn(1, 2) == &textures.texture(device, 1));
    REQUIRE(drawn(0, 0) == &textures.texture(device, 1));
    REQUIRE(drawn(2, 0) == &textures.texture(device, 0)); // no inherited sequence override
    tree.nodes.front().textureAnimation = 1;
    REQUIRE(drawn(1, 0) == &textures.texture(device, 1)); // node override takes precedence
}

} // namespace
