#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SelectionHighlight.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"

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
    test::convertModelFixture(dir);
    return dir;
}

TEST_CASE("static model textures use free clocks without sequence overrides",
          "[world][model][animation][static-texture-clock]") {
    const auto dir = sampleFigure("static-model-texture-clock");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel model;
    TreeInfo tree = trees.tree(0);
    tree.nodes[1].objectFlags = TreeNodeInfo::kNoDepthWriteFlag;
    REQUIRE(model.bind(tree, models, textures, device));
    TextureAnimationInfo cycle;
    cycle.texture = 0;
    cycle.source = 0;
    cycle.frames = 2;
    cycle.rate = 1;
    TextureAnimationInfo scroll;
    scroll.texture = 1;
    scroll.source = TextureAnimationInfo::kScrollU;
    scroll.frames = 4;
    auto vertical = scroll;
    vertical.source = TextureAnimationInfo::kScrollV;
    vertical.frames = -8;
    auto keyed = cycle;
    keyed.flag = 0;
    keyed.source = 1;
    keyed.frames = 1;
    const std::array animations{cycle, scroll, vertical, keyed};
    TextureAnimator animator;
    animator.bind(animations, textures, device);
    animator.advance(1.0f / 30.0f);
    animator.apply(model, animator.presentationOffset(0.5f));
    CHECK(model.textureOffset(1) == Vec2(0.125f, -0.0625f));
    model.draw(device, Mat4{1}, Mat4{1}, {});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].texture == &textures.texture(device, 0));
    CHECK(device.draws[0].state.nextTexture == &textures.texture(device, 1));
    CHECK(device.draws[0].state.textureBlend == 0.5f);
    // Drawing again does not advance or accumulate offsets. Native stepping omits blending.
    animator.apply(model);
    CHECK(model.textureOffset(1) == Vec2(0.25f, -0.125f));
    device.draws.clear();
    model.draw(device, Mat4{1}, Mat4{1}, {});
    CHECK(device.draws[0].texture == &textures.texture(device, 1));
    CHECK(device.draws[0].state.nextTexture == nullptr);
    CHECK(animator.frame() == 1);
    animator.step();
    animator.apply(model);
    device.draws.clear();
    model.draw(device, Mat4{1}, Mat4{1}, {});
    CHECK(device.draws[0].texture == &textures.texture(device, 0)); // not the keyed override
    animator.clear();
    animator.apply(model);
    CHECK(model.textureOffset(1) == Vec2(0));
}

TEST_CASE("world occlusion is a per-draw policy and preserves authored halo blending",
          "[world][model][enemy-projectile-depth]") {
    const auto dir = sampleFigure("tree-model-world-occlusion");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    TreeInfo tree = trees.tree(0);
    tree.nodes[2].objectFlags = TreeNodeInfo::kAdditiveFlag | TreeNodeInfo::kNoDepthWriteFlag |
                                TreeNodeInfo::kNoDepthTestFlag;
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(tree, models, textures, device));
    for (const auto policy : {TreeModel::Occlusion::Authored, TreeModel::Occlusion::SolidWorld,
                              TreeModel::Occlusion::Authored}) {
        device.draws.clear();
        figure.draw(device, Mat4{1}, Mat4{1}, {}, {}, nullptr, 1, TreeModel::Pass::All, policy);
        REQUIRE(device.draws.size() == 2);
        CHECK(device.draws[0].state.depthTest);
        CHECK(device.draws[0].state.depthWrite);
        CHECK(device.draws[1].state.depthTest == (policy == TreeModel::Occlusion::SolidWorld));
        CHECK_FALSE(device.draws[1].state.depthWrite);
        CHECK(device.draws[1].state.blend == BlendMode::Additive);
    }
}

TEST_CASE("object mesh morphing matches a frame's vertices to the next's by order, else by "
          "texture coordinate and place",
          "[world][model][presentation]") {
    // Flip-book frames were exported with their vertices in another order, split or joined
    // (the grunts' archer, the demons, the knights), with their own textures (the vermin)
    // or texture coordinates (the plants). Each vertex keeps its texture coordinate and
    // colour and moves a little from frame to frame, so its nearest such vertex is it.
    Mesh from;
    from.vertices.resize(3);
    from.vertices[0].uv = {0, 0};
    from.vertices[1].uv = {1, 0};
    from.vertices[1].position = {4, 0, 0};
    from.vertices[2].uv = {0, 1};
    from.vertices[2].position = {0, 4, 0};
    from.parts.push_back(MeshPart{0, 0, {0, 1, 2}});
    Mesh to = from;
    to.vertices[1].position.x = 4.5f;
    REQUIRE(TreeModel::compatibleMorph(from, to));
    SECTION("the same order maps as itself, whatever the coordinates, colours or textures") {
        to.vertices[1].uv.x = 0.9f;
        to.vertices[2].color.r = 0;
        to.parts[0].texture = 1;
        const auto map = TreeModel::morphMap(from, to);
        REQUIRE(map.has_value());
        CHECK(map->empty());
    }
    SECTION("another order is matched vertex by vertex") {
        std::swap(to.vertices[0], to.vertices[2]);
        to.parts[0].indices = {2, 1, 0};
        const auto map = TreeModel::morphMap(from, to);
        REQUIRE(map.has_value());
        CHECK(*map == std::vector<u32>{2, 1, 0});
    }
    SECTION("a frame with a vertex more or fewer shares what matches") {
        to.vertices.push_back(to.vertices[1]);
        to.parts[0].indices = {0, 1, 2, 0, 3, 2};
        const auto ahead = TreeModel::morphMap(from, to);
        REQUIRE(ahead.has_value());
        CHECK(*ahead == std::vector<u32>{0, 1, 2});
        const auto back = TreeModel::morphMap(to, from);
        REQUIRE(back.has_value());
        CHECK(*back == std::vector<u32>{0, 1, 2, 1});
    }
    SECTION("a vertex with no partner of its coordinates leaves the pair to cut") {
        std::swap(to.vertices[0], to.vertices[2]);
        to.parts[0].indices = {2, 1, 0};
        to.vertices[1].uv = {0.5f, 0.5f};
        CHECK_FALSE(TreeModel::morphMap(from, to).has_value());
    }
    SECTION("a partner further off than half the body is another vertex") {
        std::swap(to.vertices[0], to.vertices[2]);
        to.parts[0].indices = {2, 1, 0};
        to.vertices[1].position.x = 9;
        CHECK_FALSE(TreeModel::morphMap(from, to).has_value());
    }
    SECTION("parts and lighting must agree") {
        to.parts.push_back(MeshPart{1, 0, {0, 1, 2}});
        CHECK_FALSE(TreeModel::compatibleMorph(from, to));
        to.parts.pop_back();
        to.prelit = true;
        CHECK_FALSE(TreeModel::compatibleMorph(from, to));
    }
}

TEST_CASE("prelit tree meshes retain vertex colour while ordinary meshes use scene lighting",
          "[world][model][alpha-wall-lighting]") {
    const auto dir = sampleFigure("tree-model-baked-lighting");
    writeTextFile(dir / "models/000_BODY.obj", "v 0 0 0 0.25 0.5 0.75\nv 1 0 0 0.5 0.25 0.75\n"
                                               "v 0 1 0 0.75 0.5 0.25\nusemtl tex0\nf 1 2 3\n");
    test::convertModelFixture(dir);
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    test::FakeRenderDevice device;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    const auto body = models.find("BODY");
    REQUIRE(body);
    const auto& mesh = models.mesh(*body);
    REQUIRE(mesh.prelit);
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    const auto checkColors = [&](bool flash) {
        device.draws.clear();
        figure.draw(device, Mat4{1}, Mat4{1}, {});
        REQUIRE(device.draws.size() == 2);
        REQUIRE(device.draws[0].vertices.size() == 3);
        for (usize i = 0; i < 3; ++i) {
            CHECK(device.draws[0].vertices[i].color ==
                  (flash ? Color::white() : mesh.vertices[mesh.parts[0].indices[i]].color));
        }
        CHECK(device.draws[1].vertices[0].color ==
              (flash ? Color::white() : WorldLighting{}.shade({0, 0, 1})));
    };
    checkColors(false);
    figure.setAppearance({.unlit = true});
    checkColors(true);
    figure.setAppearance({});
    checkColors(false);
    // Darkened, the baked colour is taken down as drawn; the lit part has the darkening in
    // the light it is given, so the lanterns' light stays over it.
    WorldLighting dimmed;
    dimmed.darken = 0.5f;
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1}, dimmed);
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].state.darken == 0.5f);
    CHECK(device.draws[1].state.darken == 0.0f);
}

TEST_CASE("tree models resolve native external slots only through local named lenders",
          "[world][model][texture-lender]") {
    const auto dir = sampleFigure("tree-model-external");
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"name":"SKIN","file":"textures/000_SKIN.png","width":2,"height":2},
        {"name":"CLOTH","file":"unused.png","width":2,"height":2,"flags":32}]})");
    test::convertModelFixture(dir);
    const auto lenderDir = test::scratchDirectory("tree-model-lender");
    writeFile(lenderDir / "cloth.png", test::kTinyPng);
    writeTextFile(lenderDir / "textures.json", R"({"bitmaps":[
        {"name":"CLOTH","file":"cloth.png","width":2,"height":2,"flags":128}]})");
    test::convertModelFixture(lenderDir);
    ModelSet models;
    TextureSet textures;
    TextureSet lender;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(lender.load(lenderDir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel model;
    REQUIRE_FALSE(model.bind(trees.tree(0), models, textures, device));
    REQUIRE_FALSE(model.bound());
    const std::array<TextureSet*, 3> lenders{nullptr, &textures, &lender};
    REQUIRE(model.bind(trees.tree(0), models, textures, device, lenders));
    model.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[1].texture == &lender.texture(device, 0));
    CHECK(device.draws[1].state.blend == BlendMode::Alpha);
    // Overrides still address the costume slot, not the lender's unrelated index.
    model.setTextureFrame(1, &device.whiteTexture());
    device.draws.clear();
    model.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[1].texture == &device.whiteTexture());
    REQUIRE_FALSE(model.bind(trees.tree(0), models, textures, device));
    CHECK_FALSE(model.bound());
}

TEST_CASE("tree effects can composite after scenery without redrawing depth-writing parts",
          "[world][model][effect-pass]") {
    const auto dir = sampleFigure("tree-model-passes");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    TreeInfo tree = trees.tree(0);
    tree.nodes[2].objectFlags = TreeNodeInfo::kAdditiveFlag | TreeNodeInfo::kNoDepthWriteFlag;
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(tree, models, textures, device));
    const auto draw = [&](TreeModel::Pass pass, f32 alpha) {
        device.draws.clear();
        figure.draw(device, Mat4{1}, Mat4{1}, {}, {}, nullptr, alpha, pass);
    };
    draw(TreeModel::Pass::DepthWriting, 1);
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].state.depthWrite);
    draw(TreeModel::Pass::Effects, 1);
    REQUIRE(device.draws.size() == 1);
    CHECK_FALSE(device.draws[0].state.depthWrite);
    CHECK(device.draws[0].state.depthTest); // opaque walls must still hide the glow
    CHECK(device.draws[0].state.blend == BlendMode::Additive);
    draw(TreeModel::Pass::All, 1);
    CHECK(device.draws.size() == 2);
    draw(TreeModel::Pass::DepthWriting, 0.5f);
    CHECK(device.draws.empty());
    draw(TreeModel::Pass::Effects, 0.5f);
    CHECK(device.draws.size() == 2);
}

TEST_CASE("pickup passes separate blended mesh parts without changing native depth state",
          "[world][model][effect-pass][pickup-occlusion]") {
    const auto dir = sampleFigure("tree-model-pickup-passes");
    // The fruit and its shadow can be two materials in the very same native node; the
    // exporter marks such an object alpha-textured, and the whole of it waits.
    writeTextFile(dir / "models/000_BODY.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 1 0\n"
                  "usemtl tex0\nf 1//1 2//1 3//1\nusemtl tex1\nf 1//1 2//1 3//1\n");
    test::convertModelFixture(dir);
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    TreeInfo tree = trees.tree(0);
    tree.nodes.resize(2);
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(tree, models, textures, device));
    const auto draw = [&](TreeModel::Pass pass, f32 alpha = 1.0f) {
        device.draws.clear();
        figure.draw(device, Mat4{1}, Mat4{1}, {}, {}, nullptr, alpha, pass);
    };
    REQUIRE(models.mesh(0).alphaTextured());
    draw(TreeModel::Pass::Opaque);
    CHECK(device.draws.empty());
    draw(TreeModel::Pass::Blended);
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].texture == &textures.texture(device, 0));
    CHECK(device.draws[1].texture == &textures.texture(device, 1));
    for (const auto& part : device.draws) {
        CHECK(part.state.depthWrite);
        CHECK(part.state.depthTest);
        CHECK(part.state.blend == BlendMode::Alpha);
        CHECK(part.state.alphaTest == DrawState::kTranslucentAlphaTest);
        CHECK_FALSE(part.state.usesAlphaToCoverage(4));
    }
    // Existing actor/cutout passes keep their depth-write partition and MSAA coverage.
    draw(TreeModel::Pass::DepthWriting);
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[1].state.usesAlphaToCoverage(4));
    draw(TreeModel::Pass::Effects);
    CHECK(device.draws.empty());
    draw(TreeModel::Pass::All);
    CHECK(device.draws.size() == 2);
    draw(TreeModel::Pass::Opaque, 0.5f);
    CHECK(device.draws.empty());
    draw(TreeModel::Pass::Blended, 0.5f);
    REQUIRE(device.draws.size() == 2);
    for (const auto& part : device.draws) {
        CHECK_FALSE(part.state.depthWrite);
        CHECK_FALSE(part.state.usesAlphaToCoverage(4));
    }
    tree.nodes[1].objectFlags = TreeNodeInfo::kNoDepthWriteFlag | TreeNodeInfo::kAdditiveFlag;
    REQUIRE(figure.bind(tree, models, textures, device));
    draw(TreeModel::Pass::Opaque);
    CHECK(device.draws.empty());
    draw(TreeModel::Pass::Blended);
    REQUIRE(device.draws.size() == 2);
    for (const auto& part : device.draws) {
        CHECK_FALSE(part.state.depthWrite);
        CHECK(part.state.depthTest);
        CHECK(part.state.blend == BlendMode::Additive);
    }
    // An ordinary sort key keeps the glow ahead of the scenery's deferred queue.
    draw(TreeModel::Pass::BlendedAhead);
    CHECK(device.draws.size() == 2);
    draw(TreeModel::Pass::BlendedBehind);
    CHECK(device.draws.empty());
    // A gem's halo (0x4C00880: facing, additive, sorted, behind, no depth write) waits for
    // the scenery sorted before it and composites over it, still tested against depth.
    tree.nodes[1].objectFlags = 0x4C00880;
    REQUIRE(figure.bind(tree, models, textures, device));
    draw(TreeModel::Pass::BlendedAhead);
    CHECK(device.draws.empty());
    draw(TreeModel::Pass::BlendedBehind);
    REQUIRE(device.draws.size() == 2);
    for (const auto& part : device.draws) {
        CHECK_FALSE(part.state.depthWrite);
        CHECK(part.state.depthTest);
        CHECK(part.state.blend == BlendMode::Additive);
        CHECK_FALSE(part.state.usesAlphaToCoverage(4));
    }
    draw(TreeModel::Pass::Blended);
    CHECK(device.draws.size() == 2); // the undivided pass still draws it
    draw(TreeModel::Pass::Opaque);
    CHECK(device.draws.empty());
    CHECK(TreeModel::compositesEffects(TreeModel::Pass::Blended));
    CHECK(TreeModel::compositesEffects(TreeModel::Pass::BlendedAhead));
    CHECK_FALSE(TreeModel::compositesEffects(TreeModel::Pass::BlendedBehind));
    CHECK_FALSE(TreeModel::compositesEffects(TreeModel::Pass::Opaque));
}

TEST_CASE("only translucent pickup billboards opt in to smooth sprite filtering",
          "[world][model][smooth-sprites]") {
    const auto dir = sampleFigure("tree-smooth-sprites");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    auto tree = trees.tree(0);
    tree.nodes[1].objectFlags = 0x01000000; // native cylindrical billboard
    tree.nodes[2].objectFlags = 0x04000000; // native full billboard
    test::FakeRenderDevice device;
    TreeModel model;
    REQUIRE(model.bind(tree, models, textures, device));
    for (const auto pass : {TreeModel::Pass::All, TreeModel::Pass::DepthWriting,
                            TreeModel::Pass::Opaque, TreeModel::Pass::Blended}) {
        device.draws.clear();
        model.draw(device, Mat4{1}, Mat4{1}, {}, {}, nullptr, 1, pass);
        REQUIRE_FALSE(device.draws.empty());
        for (const auto& draw : device.draws) {
            CHECK(draw.state.usesSpriteSmoothing() == (pass == TreeModel::Pass::Blended));
        }
    }
    model.setMaskedTexture(&device.whiteTexture());
    device.draws.clear();
    model.draw(device, Mat4{1}, Mat4{1}, {}, {}, nullptr, 1, TreeModel::Pass::Blended);
    REQUIRE(device.draws.size() == 1);
    CHECK_FALSE(device.draws[0].state.usesSpriteSmoothing());
    tree.nodes[2].objectFlags = 0; // a fixed shadow or an ordinary alpha-tested model
    REQUIRE(model.bind(tree, models, textures, device));
    device.draws.clear();
    model.draw(device, Mat4{1}, Mat4{1}, {}, {}, nullptr, 1, TreeModel::Pass::Blended);
    REQUIRE(device.draws.size() == 1);
    CHECK_FALSE(device.draws[0].state.usesSpriteSmoothing());
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

TEST_CASE("a double-sided tree model draws without culling until it is cleared", "[world][model]") {
    const auto dir = sampleFigure("tree-model-double-sided");
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
    CHECK(device.draws[0].state.cullBack);
    CHECK(device.draws[1].state.cullBack);
    figure.setDoubleSided(true);
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 2);
    CHECK_FALSE(device.draws[0].state.cullBack);
    CHECK_FALSE(device.draws[1].state.cullBack);
    figure.clear();
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].state.cullBack);
}

TEST_CASE("node hit masks respect mesh scope, subtree scope and whole-body overrides",
          "[world][model]") {
    const auto dir = sampleFigure("tree-model-hit-mask");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    test::FakeTexture flash{1, 1};
    test::FakeTexture frozen{1, 1};
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    figure.setMeshMaskedTexture(1, &flash);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[0].state.maskedTexture == &flash);
    REQUIRE(device.draws[1].state.maskedTexture == nullptr);
    device.draws.clear();
    figure.setNodeMaskedTexture(1, &flash);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws[1].state.maskedTexture == &flash);
    device.draws.clear();
    figure.setMaskedTexture(&frozen);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws[0].state.maskedTexture == &frozen);
    REQUIRE(device.draws[1].state.maskedTexture == &frozen);
    device.draws.clear();
    figure.resetTextures();
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws[0].state.maskedTexture == nullptr);
    REQUIRE(device.draws[1].state.maskedTexture == nullptr);
}

TEST_CASE("authored fades hide only their subtree and reset on the next pose",
          "[world][model][animation]") {
    const auto dir = sampleFigure("tree-model-fade");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    auto tree = trees.tree(0);
    tree.sequences.emplace_back();
    tree.nodes[2].textureAnimation = 0;
    TextureAnimationInfo fade;
    fade.source = TextureAnimationInfo::kFadeOut;
    fade.flag = 0;
    fade.offset = 10;
    fade.frames = 20;
    test::FakeRenderDevice device;
    TextureAnimator animator;
    animator.bind(std::span{&fade, 1}, textures, device);
    TreeModel figure;
    REQUIRE(figure.bind(tree, models, textures, device));
    animator.apply(figure, tree, 0, 20);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[0].vertices[0].color.a == 255);
    REQUIRE(device.draws[1].vertices[0].color.a == 128);
    REQUIRE_FALSE(device.draws[1].state.depthWrite);
    device.draws.clear();
    animator.apply(figure, tree, 0, 30);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws[0].texture == &textures.texture(device, 0));
    device.draws.clear();
    animator.apply(figure, tree, 0, 0);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[1].vertices[0].color.a == 255);
}

TEST_CASE("a node's own mesh can go unseen while what hangs from it stays", "[world][model]") {
    const auto dir = sampleFigure("tree-model-mesh-alpha");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    figure.setMeshAlpha(1, 0.0f); // the body; the banner hangs from it
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].texture == &textures.texture(device, 1));
    device.draws.clear();
    figure.setMeshAlpha(1, 1.0f);
    figure.draw(device, Mat4{1}, Mat4{1});
    CHECK(device.draws.size() == 2);
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
    CHECK(device.draws[0].state.usesAlphaToCoverage(4));
    // The banner's node is not authored to sort, so it is a solid too: its cutout comes
    // from the alpha compare and coverage, not from blending.
    REQUIRE(device.draws[1].state.blend == BlendMode::Opaque);
    REQUIRE(device.draws[1].state.alphaTest == DrawState::kTranslucentAlphaTest);
    CHECK(device.draws[1].state.usesAlphaToCoverage(4));
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, {}, {}, nullptr, 0.5f);
    REQUIRE(device.draws[0].state.blend == BlendMode::Alpha);
    REQUIRE_FALSE(device.draws[0].state.depthWrite);
    CHECK_FALSE(device.draws[0].state.usesAlphaToCoverage(4));
    figure.resetTextures();
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f});
    REQUIRE(device.draws[0].state.maskedTexture == nullptr);
}

TEST_CASE("animated death skins blend their own alpha without replacing the base cutout mask",
          "[world][model][death-dissolve]") {
    const auto dir = sampleFigure("tree-model-death-dissolve");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    test::FakeTexture death{1, 1};
    death.pixels = {0, 0, 0, 0};
    figure.setMaskedTexture(&death, TreeModel::SkinAlpha::Dissolve);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    for (usize index = 0; index < device.draws.size(); ++index) {
        const auto& draw = device.draws[index];
        CHECK(draw.texture == &textures.texture(device, static_cast<u32>(index)));
        CHECK(draw.state.maskedTexture == &death);
        CHECK(draw.state.blend == BlendMode::Alpha);
        CHECK(draw.state.alphaTest == DrawState::kTranslucentAlphaTest);
        CHECK_FALSE(draw.state.depthWrite);
        CHECK_FALSE(draw.state.alphaToCoverage);
        CHECK_FALSE(draw.state.usesAlphaToCoverage(4));
    }
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1}, {}, {}, nullptr, 1, TreeModel::Pass::DepthWriting);
    CHECK(device.draws.empty());
    figure.draw(device, Mat4{1}, Mat4{1}, {}, {}, nullptr, 1, TreeModel::Pass::Effects);
    REQUIRE(device.draws.size() == 2);
    for (const auto& draw : device.draws) {
        CHECK(draw.state.blend == BlendMode::Alpha);
        CHECK_FALSE(draw.state.depthWrite);
    }
    test::FakeTexture flash{1, 1};
    figure.setMeshMaskedTexture(1, &flash);
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].state.maskedTexture == &flash);
    CHECK(device.draws[0].state.blend == BlendMode::Opaque);
    CHECK(device.draws[0].state.depthWrite);
    CHECK(device.draws[0].state.usesAlphaToCoverage(4));
    CHECK(device.draws[1].state.maskedTexture == &death);
    CHECK_FALSE(device.draws[1].state.usesAlphaToCoverage(4));
    figure.setMeshMaskedTexture(1, nullptr);
    // Opting out or resetting must restore the original opaque-flash semantics.
    figure.setMaskedTexture(&death);
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].state.blend == BlendMode::Opaque);
    CHECK(device.draws[0].state.depthWrite);
    CHECK(device.draws[0].state.usesAlphaToCoverage(4));
    figure.setMaskedTexture(&death, TreeModel::SkinAlpha::Dissolve);
    figure.resetTextures();
    figure.setMeshMaskedTexture(1, &death);
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].state.maskedTexture == &death);
    CHECK(device.draws[0].state.blend == BlendMode::Opaque);
    CHECK(device.draws[0].state.usesAlphaToCoverage(4));
}

TEST_CASE("node texture changes affect descendants without leaking to ancestors and reset cleanly",
          "[world][model][animation]") {
    const auto dir = sampleFigure("tree-subtree-textures");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    test::FakeTexture replacement{1, 1};
    figure.setNodeTextureFrame(2, 1, &replacement);
    figure.setNodeTextureOffset(2, Vec2{0.25f, 0}, Vec2{0.5f, 1});
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[0].state.uvScale == Vec2(1));
    REQUIRE(device.draws[0].texture == &textures.texture(device, 0));
    REQUIRE(device.draws[1].state.uvOffset == Vec2(0.25f, 0));
    REQUIRE(device.draws[1].texture == &replacement);
    figure.setNodeTextureOffset(0, Vec2{0.5f, 0}, Vec2{0, 1});
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws[0].state.uvScale == Vec2(0, 1));
    REQUIRE(device.draws[1].state.uvScale == Vec2(0, 1));
    figure.resetTextures();
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws[0].state.uvOffset == Vec2(0));
    REQUIRE(device.draws[1].state.uvScale == Vec2(1));
    REQUIRE(device.draws[1].texture == &textures.texture(device, 1));
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
    figure.setAppearance({.unlit = true, .tint = blue, .depthWrite = false});
    figure.setWhiteBlend(0.4f);
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, dark);
    REQUIRE(device.draws[0].vertices[0].color == blue);
    REQUIRE_FALSE(device.draws[0].state.depthWrite);
    REQUIRE(device.draws[0].state.whiteBlend == 0.4f);
    // A new binding resets instance overrides.
    REQUIRE(figure.bind(trees.tree(0), models, textures, device));
    device.draws.clear();
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, dark);
    REQUIRE(device.draws[0].vertices[0].color == Color::black());
    REQUIRE(device.draws[0].state.depthWrite);
    REQUIRE(device.draws[0].state.whiteBlend == 0);
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
    REQUIRE(device.draws[0].state.alphaTest == DrawState::kTranslucentAlphaTest);
    // The banner has different UVs/materials. Opt-in sampling must leave this
    // incompatible pair at its discrete lower frame, not morph unrelated vertices.
    const auto discrete = device.draws[0];
    device.draws.clear();
    flame.setPresentationFrame(0, 1.5f);
    flame.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].texture == discrete.texture);
    REQUIRE(device.draws[0].vertices.size() == discrete.vertices.size());
    for (usize vertex = 0; vertex < discrete.vertices.size(); ++vertex) {
        CHECK(device.draws[0].vertices[vertex].position == discrete.vertices[vertex].position);
    }
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

    // One branch's clock may differ from the body's. An unrelated branch must
    // not change this object, and a subtree override must survive until drawing.
    flame.setSubtreeFrame(99, 0, 1);
    flame.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    flame.setSubtreeFrame(0, 0, 2);
    flame.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 3);
    REQUIRE(device.draws.back().state.alphaTest > 0);

    // Reverse applies to the sequence frame BEFORE subtracting the run's start.
    TreeInfo reversed = trees.tree(0);
    reversed.sequences[0].flags |= 1U;
    REQUIRE(flame.bind(reversed, models, textures, device));
    device.draws.clear();
    flame.setFrame(0, 3); // sequence frame 2 -> banner
    flame.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].state.alphaTest > 0);
    flame.setFrame(0, 4); // sequence frame 1 -> body
    flame.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[1].state.alphaTest == DrawState::kTranslucentAlphaTest);
    flame.setFrame(0, 5); // sequence frame 0 -> before the run
    flame.draw(device, Mat4{1}, Mat4{1});
    CHECK(device.draws.size() == 2);
}

TEST_CASE("the spawn effect's flame column comes and goes with its frames",
          "[world][model][assets]") {
    const std::filesystem::path dir = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path();
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

TEST_CASE("decoded discrete object swaps do not morph compatible geometry",
          "[world][model][object-frames]") {
    const auto directory = sampleFigure("tree-discrete-object-frames");
    ModelSet source;
    TextureSet textures;
    REQUIRE(source.load(directory));
    REQUIRE(textures.load(directory));
    std::vector<Mesh> meshes{source.mesh(0), source.mesh(0)};
    for (auto& vertex : meshes[1].vertices) {
        vertex.position.x += 10;
    }
    auto entries = std::vector<ModelSetEntry>{source.entry(0), source.entry(0)};
    entries[0].name = "FIRST";
    entries[1].name = "SECOND";
    auto models = ModelSet::fromDecoded(std::move(entries), std::move(meshes));
    TreeInfo tree;
    tree.name = "SWAP";
    tree.sequences.resize(1);
    tree.sequences[0].frames = 2;
    tree.nodes.resize(1);
    tree.nodes[0].objectFrames.push_back({"FIRST", 0, 2, false});
    test::FakeRenderDevice device;
    TreeModel figure;
    REQUIRE(figure.bind(tree, models, textures, device));
    figure.setPresentationFrame(0, .75f);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].vertices[0].position == models.mesh(0).vertices[0].position);
    device.draws.clear();
    figure.setPresentationFrame(0, 1.0f);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].vertices[0].position == models.mesh(1).vertices[0].position);
    tree.nodes[0].objectFrames[0].interpolate = true;
    REQUIRE(figure.bind(tree, models, textures, device));
    device.draws.clear();
    figure.setPresentationFrame(0, .5f);
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].vertices[0].position.x ==
          Approx(models.mesh(0).vertices[0].position.x + 5));
}

TEST_CASE("a tree model adds glowing nodes onto the frame without writing depth",
          "[world][model]") {
    const auto dir = sampleFigure("tree-model-glow");
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "FIGURE", "prefix": "", "sequences": [], "nodes": [
    {"name": "BODY", "object": "BODY", "type": 0, "flags": 0, "objectFlags": 0, "parent": -1,
     "position": [0.0, 0.0, 0.0]},
    {"name": "GLOW", "object": "BODY", "type": 0, "flags": 0, "objectFlags": 8388800,
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
    // The body first, with the console's alpha compare like everything else; the glow after
    // it, added and leaving depth alone.
    REQUIRE(device.draws[0].state.blend == BlendMode::Alpha);
    REQUIRE(device.draws[0].state.depthWrite);
    REQUIRE(device.draws[0].state.alphaTest == DrawState::kTranslucentAlphaTest);
    REQUIRE(device.draws[0].state.depthTest);
    REQUIRE(device.draws[1].state.blend == BlendMode::Additive);
    REQUIRE_FALSE(device.draws[1].state.depthWrite);
    REQUIRE_FALSE(device.draws[1].state.depthTest);
    REQUIRE(device.draws[1].state.alphaTest == DrawState::kTranslucentAlphaTest);
    REQUIRE(device.draws[1].vertices[0].position == Vec3{0.0f, 2.0f, 0.0f});
    const Texture& first = textures.texture(device, 0);
    const Texture& second = textures.texture(device, 1);
    figure.setTextureFrame(0, &first, &second, 0.5f);
    figure.setTextureFrame(1, &first, &second, 0.5f);
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].state.nextTexture == nullptr);
    CHECK(device.draws[1].state.nextTexture == &second);
    CHECK(device.draws[1].state.textureBlend == 0.5f);
    figure.setMaskedTexture(&first);
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1});
    CHECK(device.draws[1].state.nextTexture == nullptr);
    figure.resetTextures();
    device.draws.clear();
    figure.draw(device, Mat4{1}, Mat4{1});
    CHECK(device.draws[1].state.nextTexture == nullptr);
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
    CHECK(device.draws[0].state.mipmaps); // filtering does not depend on a billboard camera
    device.draws.clear();
    // A camera off to +x: the body's z axis swings to point at it, so its x edge lies along -z.
    CameraFrame camera;
    camera.position = Vec3{10.0f, 0.0f, 0.0f};
    camera.right = Vec3{0.0f, 0.0f, -1.0f};
    camera.forward = Vec3{-1.0f, 0.0f, 0.0f};
    figure.draw(device, Mat4{1.0f}, Mat4{1.0f}, WorldLighting{}, {}, &camera);
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].state.mipmaps);
    CHECK(device.draws[1].state.mipmaps);
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

TEST_CASE("keyed soft flipbooks retain discrete frames without presentation sampling",
          "[world][animation][texture-presentation]") {
    const auto dir = sampleFigure("tree-keyed-native-frames");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    TreeInfo tree = trees.tree(0);
    tree.nodes[2].objectFlags = TreeNodeInfo::kAdditiveFlag;
    tree.sequences.resize(1);
    SECTION("sequence override") {
        tree.sequences[0].textureAnimationStart = 0;
        tree.sequences[0].textureAnimationCount = 1;
    }
    SECTION("subtree override") {
        tree.nodes[2].textureAnimation = 0;
    }
    TextureAnimationInfo animation;
    animation.texture = 1;
    animation.source = 0;
    animation.frames = 2;
    animation.rate = 2;
    animation.flag = 0;
    test::FakeRenderDevice device;
    TreeModel model;
    REQUIRE(model.bind(tree, models, textures, device));
    TextureAnimator animator;
    animator.bind(std::array{animation}, textures, device);
    REQUIRE(animator.motionAt(0, 1.5f).has_value());
    CHECK(animator.motionAt(0, 1.5f)->frameBlend == Approx(0.75f));
    for (const bool presentation : {false, true, false}) {
        device.draws.clear();
        animator.apply(model, tree, 0, 1.0f,
                       presentation ? std::optional<f32>{0.0f} : std::nullopt);
        model.draw(device, Mat4{1}, Mat4{1});
        REQUIRE(device.draws.size() == 2);
        const auto& draw = device.draws.back();
        CHECK(draw.texture == &textures.texture(device, 0));
        CHECK(draw.state.nextTexture == (presentation ? &textures.texture(device, 1) : nullptr));
        CHECK(draw.state.textureBlend == (presentation ? 0.5f : 0.0f));
    }
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
    tree.sequences[1].frames = 4;
    tree.sequences[1].flags = 1;
    CHECK(drawn(1, 0) == &textures.texture(device, 1));
    CHECK(drawn(1, 3) == &textures.texture(device, 0));
    tree.sequences[1].flags = 0;
    tree.nodes.front().textureAnimation = 1;
    REQUIRE(drawn(1, 0) == &textures.texture(device, 1)); // node override takes precedence
}

} // namespace

namespace {

/** A lava pool: a square pad and a mound rising from it (texture 0) under a slightly larger
 * mask plate whose indexed picture (texture 1) shows nothing but holds a centred square. */
std::filesystem::path poolFigure(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/000_POOL.obj",
                  "v -2 0 -2\nv 2 0 -2\nv 2 0 2\nv -2 0 2\n"
                  "v -2.2 0.15 -2.2\nv 2.2 0.15 -2.2\nv 2.2 0.15 2.2\nv -2.2 0.15 2.2\n"
                  "v -1 0.1 -1\nv 1 0.1 -1\nv 0 3 0\n"
                  "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 1 0\n"
                  "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n"
                  "f 9/1/1 10/2/1 11/3/1\n"
                  "usemtl tex1\nf 5/1/1 6/2/1 7/3/1\nf 5/1/1 7/3/1 8/4/1\n");
    writeTextFile(dir / "objects.json", R"({"objects": [
  {"index": 0, "name": "POOL", "file": "models/000_POOL.obj", "meshTriangles": 5}]})");
    writeFile(dir / "textures/000_LAVA.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"defs": [], "bitmaps": [
  {"index": 0, "name": "LAVA", "file": "textures/000_LAVA.png", "width": 8, "height": 8, "flags": 0},
  {"index": 1, "name": "", "width": 8, "height": 8, "flags": 140,
   "silhouette": ["00000000", "00111100", "00111100", "00111100",
                  "00111100", "00111100", "00111100", "00000000"]}]})");
    writeTextFile(dir / "animations.json", R"({"trees": [
  {"name": "POOL", "prefix": "", "sequences": [], "nodes": [
    {"name": "POOL", "object": "POOL", "type": 0, "flags": 0, "objectFlags": 0, "parent": -1,
     "position": [0.0, 0.0, 0.0]}]}]})");
    test::convertModelFixture(dir);
    return dir;
}

} // namespace

TEST_CASE("an authored silhouette cuts the flat part beneath it instead of drawing",
          "[world][model][silhouette]") {
    const auto dir = poolFigure("tree-model-silhouette");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel model;
    REQUIRE(model.bind(trees.tree(0), models, textures, device));
    device.draws.clear();
    model.draw(device, Mat4{1}, Mat4{1});
    // The mask plate is not drawn; the pad is, cut by the mask's shape.
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws[0];
    CHECK(draw.texture == &textures.texture(device, 0));
    CHECK(draw.state.lightmapCutout);
    CHECK(draw.state.lightmap == textures.silhouetteTexture(device, 1));
    REQUIRE(draw.vertices.size() == 9);
    // The mound rising from the pad keeps whole: its three corners share one set texel.
    std::vector<Vec2> mound;
    for (const ImmediateVertex& v : draw.vertices) {
        if (v.position.y > 0.05f) {
            mound.push_back(v.uv2);
        }
    }
    REQUIRE(mound.size() == 3);
    CHECK(mound[1] == mound[0]);
    CHECK(mound[2] == mound[0]);
    CHECK(mound[0].x == Approx(0.5f).margin(0.07f));
    CHECK(mound[0].y == Approx(0.5f).margin(0.07f));
    // The pad's corner (-2, -2) sits 0.2 of 4.4 units into the plate from its corner, whose
    // picture coordinate the fixture flips down (OBJ counts v from the bottom); the archive
    // keeps 1/128 of a unit.
    const auto corner = std::ranges::find_if(draw.vertices, [](const ImmediateVertex& v) {
        return v.position.x < -1.9f && v.position.z < -1.9f;
    });
    REQUIRE(corner != draw.vertices.end());
    CHECK(corner->uv2.x == Approx(0.2f / 4.4f).margin(2e-3f));
    CHECK(corner->uv2.y == Approx(1.0f - 0.2f / 4.4f).margin(2e-3f));
    for (const ImmediateVertex& v : draw.vertices) {
        CHECK(v.uv2.x >= 0.0f);
        CHECK(v.uv2.x <= 1.0f);
        CHECK(v.uv2.y >= 0.0f);
        CHECK(v.uv2.y <= 1.0f);
    }
}

TEST_CASE("a figure without silhouette masks draws every part uncut",
          "[world][model][silhouette]") {
    const auto dir = sampleFigure("tree-model-no-silhouette");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel model;
    REQUIRE(model.bind(trees.tree(0), models, textures, device));
    device.draws.clear();
    model.draw(device, Mat4{1}, Mat4{1});
    REQUIRE(device.draws.size() == 2);
    for (const auto& draw : device.draws) {
        CHECK_FALSE(draw.state.lightmapCutout);
        CHECK(draw.state.lightmap == nullptr);
    }
}

TEST_CASE("a highlighted figure draws each part's rim and wash after the part",
          "[world][model][dev-console]") {
    const auto dir = sampleFigure("highlighted-figure");
    ModelSet models;
    TextureSet textures;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    TreeModel model;
    REQUIRE(model.bind(trees.tree(0), models, textures, device));
    CHECK(model.highlight() == nullptr);
    model.draw(device, Mat4{1}, Mat4{1}, {});
    const usize plain = device.draws.size();
    REQUIRE(plain == 2);
    const SelectionHighlight highlight{Vec3{0.0f, 0.0f, -10.0f}};
    model.setHighlight(&highlight);
    CHECK(model.highlight() == &highlight);
    device.draws.clear();
    model.draw(device, Mat4{1}, Mat4{1}, {});
    REQUIRE(device.draws.size() == plain * 3);
    for (usize part = 0; part < plain; ++part) {
        const auto& rim = device.draws[part * 3 + 1];
        const auto& wash = device.draws[part * 3 + 2];
        CHECK(rim.texture == device.draws[part * 3].texture);
        CHECK(rim.state.maskedTexture == &device.whiteTexture());
        REQUIRE_FALSE(rim.vertices.empty());
        CHECK(rim.vertices[0].color == SelectionHighlight::kRim);
        CHECK(wash.texture == device.draws[part * 3].texture);
        REQUIRE_FALSE(wash.vertices.empty());
        CHECK(wash.vertices[0].color == SelectionHighlight::kWash);
        CHECK_FALSE(rim.state.depthWrite);
        CHECK_FALSE(wash.state.depthWrite);
    }
    // Let go, the parts draw alone again; cleared, the model forgets the highlight.
    model.setHighlight(nullptr);
    device.draws.clear();
    model.draw(device, Mat4{1}, Mat4{1}, {});
    CHECK(device.draws.size() == plain);
    model.setHighlight(&highlight);
    model.clear();
    CHECK(model.highlight() == nullptr);
}
