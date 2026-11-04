#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/world/SelectionHighlight.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/ThermalMaterial.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "SampleLevel.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"

namespace {

using namespace gdl;
using Catch::Approx;

/** The sample's burning surface, as a game names it: the torch's lent TORCHB. */
constexpr std::array<std::string_view, 1> kSampleFlames{"TORCHB"};
constexpr ThermalMaterial kSampleThermal{{}, kSampleFlames};

struct Fixture {
    std::filesystem::path directory;
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    TextureSet lender;
    WorldLayout layout;
    WorldScene scene;

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        directory = dir;
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
        REQUIRE(lender.load(test::sampleLender(std::string(name) + "-lender")));
    }

    bool build(std::span<const usize> controlled = {}, std::span<const usize> background = {},
               std::span<const usize> cameraFade = {}) {
        const std::array<TextureSet*, 1> lenders{&lender};
        return scene.build(layout, models, textures, device, WorldLighting{}, lenders, controlled,
                           background, cameraFade, nullptr, kSampleThermal);
    }
};

struct DigestTextures {
    std::vector<std::pair<const Texture*, u32>> slots;

    explicit DigestTextures(Fixture& fixture) {
        for (u32 slot = 0; slot < 3; ++slot) {
            slots.emplace_back(&fixture.textures.texture(fixture.device, slot), slot);
        }
        for (u32 slot = 0; slot < 4; ++slot) {
            slots.emplace_back(&fixture.lender.texture(fixture.device, slot), 100 + slot);
        }
        slots.emplace_back(&fixture.device.whiteTexture(), 200);
    }
    NativeStateDigest::Resources resources() {
        return {this, [](NativeStateDigest& digest, std::type_index type, const void* pointer,
                         void* context) {
                    if (type != typeid(Texture)) {
                        return false;
                    }
                    const auto& bindings = *static_cast<DigestTextures*>(context);
                    for (const auto& [texture, identity] : bindings.slots) {
                        if (texture == pointer) {
                            digest.word(identity);
                            return true;
                        }
                    }
                    return false;
                }};
    }
};

std::optional<NativeStateDigest::Digest>
sceneDigest(const WorldScene& scene, NativeStateDigest::Resources resources = {},
            const WorldScene::LocalSnapshot* saved = nullptr) {
    NativeStateDigest digest(resources);
    if (saved) {
        scene.appendLocalDigest(digest, *saved);
    } else {
        scene.appendNativeDigest(digest);
    }
    return digest.checkedFinish();
}

TEST_CASE("geometry checkpoints restore hierarchy and visibility without retaining stale overrides",
          "[world][scene][geometry-replica]") {
    Fixture f("world-scene-replica");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"GROUP","position":[10,0,0],"next":2,"child":1,"flags":4096},
      {"name":"WINDOW","position":[0,5,0],"next":-1,"child":-1},
      {"name":"WALL","position":[20,0,0],"next":-1,"child":-1}]})");
    REQUIRE(f.layout.load(f.directory));
    const std::array<usize, 1> controlled{2};
    REQUIRE(f.build(controlled));
    auto replica = f.scene;
    const auto initial = f.scene.geometry();
    REQUIRE(initial.valid());
    REQUIRE(initial.objects.size() == 2); // includes the meshless moving parent
    REQUIRE(replica.acceptsGeometry(initial));
    const Mat4 moved =
        glm::rotate(glm::translate(Mat4{1}, Vec3{10, 12, 0}), kHalfPi, Vec3{0, 0, 1});
    f.scene.setObjectTransform(0, moved);
    f.scene.setObjectAlpha(1, 0.25f);
    REQUIRE(f.scene.setObjectVisible(2, false));
    f.scene.setDarken(0.5f);
    const auto changed = f.scene.geometry();
    REQUIRE(changed.objects.size() == 3);
    REQUIRE(replica.applyGeometry(changed));
    CHECK(replica.worldTransform(1) == f.scene.worldTransform(1));
    CHECK(replica.objectAlpha(1) == 0.25f);
    CHECK_FALSE(replica.objectVisible(2));
    CHECK(replica.geometry().darken == 0.5f);
    const auto textures = f.device.texturesCreated;
    REQUIRE(replica.applyGeometry(initial));
    CHECK(replica.objectVisible(2));
    CHECK(replica.objectAlpha(1) == 1);
    CHECK(replica.geometry().darken == 0);
    CHECK(replica.worldTransform(1)[3] == Vec4(10, 5, 0, 1));
    CHECK(f.device.texturesCreated == textures);
    CHECK_FALSE(f.scene.objectVisible(2)); // independent renderer, not host mutation

    auto invalid = changed;
    SECTION("missing moving parent") {
        invalid.objects.erase(invalid.objects.begin());
    }
    SECTION("wrong layout") {
        ++invalid.layout;
    }
    SECTION("wrong object count") {
        ++invalid.objectCount;
    }
    SECTION("moving static geometry") {
        invalid.objects.back().local[3].y += 1;
    }
    SECTION("meshless visibility") {
        invalid.objects.front().visible = false;
    }
    SECTION("invalid trailing record") {
        invalid.objects.back().alpha = -1;
    }
    CHECK_FALSE(replica.applyGeometry(invalid));
    CHECK(replica.worldTransform(1)[3] == Vec4(10, 5, 0, 1));
    CHECK(replica.objectAlpha(1) == 1);
    CHECK(replica.objectVisible(2));
    CHECK(replica.geometry().darken == 0);
}

TEST_CASE("geometry cuts carry a discontinuity distinct from ordinary platform updates",
          "[world][scene][geometry-replica]") {
    Fixture f("world-scene-replica-cuts");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[0,0,0],"next":-1,"child":-1,"flags":4096}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    REQUIRE(f.scene.geometry().objects.size() == 1);
    const Mat4 moved = glm::translate(Mat4{1}, Vec3{0, 5, 0});
    f.scene.setObjectTransform(0, moved);
    CHECK(f.scene.geometry().objects[0].continuity == 1);
    f.scene.setObjectTransform(0, moved, true);
    CHECK(f.scene.geometry().objects[0].continuity == 1);
    f.scene.setObjectTransform(0, Mat4{1}, true);
    CHECK(f.scene.geometry().objects[0].continuity == 2);
}

TEST_CASE("placed flame surfaces produce separate depth-tested heat sources",
          "[world][scene][heat-surfaces]") {
    Fixture f("world-scene-heat-surfaces");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"FLAME","position":[-4,0,0],"next":1,"child":-1},
      {"name":"FLAME","position":[4,0,0],"next":2,"child":-1},
      {"name":"WALL","position":[0,0,0],"next":-1,"child":-1}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    REQUIRE(f.scene.unitCount() == 2);  // same material must not combine distant flames
    REQUIRE(f.scene.batchCount() == 1); // stone stays batched and does not emit heat
    Mat4 clip = glm::scale(Mat4{1}, Vec3{0.1f});
    clip[3].z = 0.5f;
    REQUIRE(f.device.beginFrame());
    f.scene.draw(f.device, clip, CameraFrame{});
    const auto sources = f.device.heat.sources();
    REQUIRE(sources[0].radius.x > 0);
    REQUIRE(sources[1].radius.x > 0);
    CHECK(sources[2].radius == Vec2{0});
    CHECK(sources[0].center.x == Approx(0.325f));
    CHECK(sources[1].center.x == Approx(0.725f));
    CHECK(sources[0].radius.x < 0.05f);
    CHECK(sources[0].center.y > 0.525f); // distortion rises just above the flame
    CHECK(sources[0].depth == Approx(0.5f));
    REQUIRE(f.device.draws.size() == 3);
    for (const auto& draw : f.device.draws) {
        CHECK(draw.state.depthTest); // walls in front still occlude both flame and heat
    }

    REQUIRE(f.scene.setObjectVisible(0, false));
    f.scene.setObjectAlpha(1, 0);
    REQUIRE(f.device.beginFrame());
    f.scene.draw(f.device, clip, CameraFrame{});
    CHECK(f.device.heat.sources()[0].radius == Vec2{0});

    // Authored depthless scenery and explicitly selected backdrops must not
    // generate a foreground shimmer, nor may an unresolved white placeholder.
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"FLAME","position":[-4,0,0],"next":1,"child":-1,"objectFlags":16777280},
      {"name":"FLAME","position":[4,0,0],"next":-1,"child":-1}]})");
    REQUIRE(f.layout.load(f.directory));
    const std::array<usize, 1> background{1};
    REQUIRE(f.build({}, background));
    REQUIRE(f.device.beginFrame());
    f.scene.draw(f.device, clip, CameraFrame{});
    CHECK(f.device.heat.sources()[0].radius == Vec2{0});
    REQUIRE(f.scene.build(f.layout, f.models, f.textures, f.device, {}, {}, {}, {}, {}, nullptr,
                          kSampleThermal));
    REQUIRE(f.device.beginFrame());
    f.scene.draw(f.device, clip, CameraFrame{});
    CHECK(f.device.heat.sources()[0].radius == Vec2{0});
}

TEST_CASE("a scene told of no burning surfaces batches its flames and adds no heat",
          "[world][scene][heat-surfaces]") {
    Fixture f("world-scene-cold-surfaces");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"FLAME","position":[-4,0,0],"next":1,"child":-1},
      {"name":"FLAME","position":[4,0,0],"next":2,"child":-1},
      {"name":"WALL","position":[0,0,0],"next":-1,"child":-1}]})");
    REQUIRE(f.layout.load(f.directory));
    const std::array<TextureSet*, 1> lenders{&f.lender};
    REQUIRE(f.scene.build(f.layout, f.models, f.textures, f.device, WorldLighting{}, lenders));
    CHECK(f.scene.unitCount() == 0); // the engine knows no flame by name
    Mat4 clip = glm::scale(Mat4{1}, Vec3{0.1f});
    clip[3].z = 0.5f;
    REQUIRE(f.device.beginFrame());
    f.scene.draw(f.device, clip, CameraFrame{});
    CHECK(f.device.heat.sources()[0].radius == Vec2{0});
}

TEST_CASE("flame heat follows animated scenery and its interpolated texture clock",
          "[world][scene][heat-surfaces][interpolation]") {
    Fixture f("world-scene-heat-clock");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"FLAME","position":[0,0,0],"next":-1,"child":-1,"flags":4096}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    TextureAnimator animator;
    animator.bind({}, f.textures, f.device);
    animator.advance(1.0f / 30.0f);
    animator.apply(f.scene);
    f.scene.capturePresentation();
    f.scene.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{2, 0, 0}));
    const Mat4 native = f.scene.worldTransform(0);
    Mat4 clip = glm::scale(Mat4{1}, Vec3{0.1f});
    clip[3].z = 0.5f;
    const auto sample = [&](f32 alpha) {
        REQUIRE(f.device.beginFrame());
        f.scene.draw(f.device, clip, CameraFrame{}, alpha, animator.presentationOffset(alpha));
        return f.device.heat.sources()[0];
    };
    CHECK(sample(0).center.x == Approx(0.525f));
    const HeatSource half = sample(0.5f);
    CHECK(half.center.x == Approx(0.575f));
    CHECK(half.seconds == Approx(1.0f / 60.0f));
    CHECK(sample(1).center.x == Approx(0.625f));
    CHECK(sample(1).seconds == Approx(1.0f / 30.0f));
    CHECK(sample(0.5f).center == half.center);
    CHECK(sample(0.5f).seconds == half.seconds);
    CHECK(animator.frame() == 1);
    CHECK(f.scene.worldTransform(0) == native);
    animator.advance(0);
    f.scene.capturePresentation();
    for (const f32 alpha : {-1.0f, 0.0f, 0.5f, 1.0f}) {
        CHECK(sample(alpha).center.x == Approx(0.625f));
        CHECK(sample(alpha).seconds == Approx(1.0f / 30.0f));
    }
    f.scene.clear();
    REQUIRE(f.build());
    CHECK(sample(-1).seconds == 0); // another level does not inherit the previous clock
}

TEST_CASE("controlled static geometry hides only its own mesh without moving or fading",
          "[world][scene][boss-effects]") {
    Fixture f("world-scene-controlled");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[10,0,0],"next":2,"child":1},
      {"name":"WINDOW","position":[0,5,0],"next":-1,"child":-1},
      {"name":"WALL","position":[20,0,0],"next":-1,"child":-1}]})");
    REQUIRE(f.layout.load(f.directory));
    const std::array<usize, 1> controlled{0};
    REQUIRE(f.build(controlled));
    REQUIRE(f.scene.unitCount() == 1);
    REQUIRE_FALSE(f.scene.moving(0));
    const Mat4 placement = f.scene.worldTransform(0);
    f.scene.setObjectTransform(0, Mat4{1});
    REQUIRE(f.scene.worldTransform(0) == placement);
    f.scene.draw(f.device, Mat4{1});
    REQUIRE(f.device.draws.size() == 3);
    REQUIRE(f.scene.setObjectVisible(0, false));
    REQUIRE_FALSE(f.scene.objectVisible(0));
    REQUIRE(f.scene.objectVisible(1));
    REQUIRE(f.scene.objectAlpha(0) == 1);
    REQUIRE_FALSE(f.scene.setObjectVisible(2, false)); // baked, same texture as the hidden mesh
    REQUIRE_FALSE(f.scene.setObjectVisible(99, false));
    REQUIRE_FALSE(f.scene.objectVisible(99));
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1});
    REQUIRE(f.device.draws.size() == 2);
    REQUIRE(f.device.draws[0].vertices[0].position == Vec3{20, 0, 0});
    REQUIRE(f.device.draws[1].vertices[0].position == Vec3{10, 5, 0}); // the veil child still draws
    REQUIRE(f.scene.setObjectVisible(0, true));
    f.scene.setObjectAlpha(0, 0.5f);
    REQUIRE(f.scene.setObjectVisible(0, false));
    REQUIRE(f.scene.setObjectVisible(0, true));
    REQUIRE(f.scene.objectAlpha(0) == 0.5f); // visibility does not replace a fade
    REQUIRE(f.scene.setObjectVisible(0, false));
    REQUIRE(f.build(controlled));
    REQUIRE(f.scene.objectVisible(0)); // a new scene restores the object
}

TEST_CASE("an overlay authored without a depth compare is tested and lifted toward the eye",
          "[world][scene][overlay]") {
    // A3XOEXTRACTED_P lies on the Armory rack's face with the no-compare, no-write, sorted
    // and alpha-last words; the original paints it through the parapet in front of it.
    Fixture f("world-scene-overlay");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[0,0,0],"next":1,"child":-1},
      {"name":"PANE","position":[0,0,5],"next":-1,"child":-1,"objectFlags":526528}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    REQUIRE(f.scene.unitCount() == 1);
    f.scene.draw(f.device, Mat4{1}, CameraFrame::at(Vec3{0, 0, -10}));
    REQUIRE(f.device.draws.size() == 2);
    const auto& wall = f.device.draws[0];
    CHECK(wall.vertices[0].position == Vec3{0, 0, 0});
    CHECK(wall.state.depthTest);
    const auto& overlay = f.device.draws[1];
    CHECK(overlay.state.depthTest);
    CHECK_FALSE(overlay.state.depthWrite);
    // Lifted a tenth toward the eye at (0, 0, -10): straight back along z.
    CHECK(overlay.vertices[0].position.x == Approx(0));
    CHECK(overlay.vertices[0].position.y == Approx(0));
    CHECK(overlay.vertices[0].position.z == Approx(5 - WorldScene::kOverlayLift));
    CHECK(overlay.vertices[1].position.z < 5);
}

TEST_CASE("a world scene places every object that has a mesh and draws it in passes",
          "[world][scene]") {
    Fixture f("world-scene");
    REQUIRE(f.build());
    REQUIRE(f.scene.built());
    // The particle marker and the meshless objects are not placed; the torch, the panes and
    // the blade under the spinning group are units of their own.
    REQUIRE(f.scene.placedCount() == 8);
    REQUIRE(f.scene.batchCount() == 4);
    REQUIRE(f.scene.unitCount() == 4);
    REQUIRE(f.scene.triangleCount() == 8);
    REQUIRE(f.scene.moving(8));
    REQUIRE(f.scene.moving(10));
    REQUIRE_FALSE(f.scene.moving(2));
    REQUIRE_FALSE(f.scene.moving(99));

    f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 0.0f});
    REQUIRE(f.device.draws.size() == 8);
    for (const auto& draw : f.device.draws) {
        CHECK(draw.state.mipmaps); // baked batches and individually animated scenery
    }
    // The solid batches draw in object order: the wall offset by its parent, then the
    // lightmapped floor (its own batch under the wall's texture); the moving blade follows;
    // the window (alpha-textured but not authored to sort, so a solid, though a veil for
    // its translucent texture) comes after the figures' turn; the sorted panes come
    // farthest first; the flame and the glowing copy of the wall add themselves last.
    const auto& window = f.device.draws[3];
    REQUIRE(window.vertices[0].position == Vec3{10.0f, 5.0f, 0.0f});
    REQUIRE(window.state.alphaTest == WorldScene::kAlphaTest);
    REQUIRE(window.state.depthWrite);
    const auto& wall = f.device.draws[0];
    REQUIRE(wall.vertices.size() == 3);
    REQUIRE(wall.vertices[0].position == Vec3{11.0f, 0.0f, 0.0f});
    REQUIRE(wall.vertices[1].position == Vec3{12.0f, 0.0f, 0.0f});
    REQUIRE(wall.blend() == BlendMode::Alpha);
    REQUIRE(wall.lightmap() == nullptr);
    REQUIRE(wall.state.cullBack);
    REQUIRE(wall.state.depthWrite);
    REQUIRE(wall.state.alphaTest == WorldScene::kAlphaTest);
    const auto& floor = f.device.draws[1];
    REQUIRE(floor.texture == wall.texture);
    REQUIRE(floor.lightmap() == &f.textures.texture(f.device, 2));
    REQUIRE(floor.vertices[0].position == Vec3{10.0f, 0.0f, 20.0f}); // under the group
    REQUIRE(floor.vertices[1].uv2 == Vec2{0.5f, 0.25f});             // texels over the 2x2 lightmap
    const auto& blade = f.device.draws[2];
    REQUIRE(blade.texture == wall.texture);
    REQUIRE(blade.vertices[0].position == Vec3{11.0f, 0.0f, 50.0f});
    // Level geometry is never lit by the level's light (every world node is NOSHADING):
    // without a colour stream the wall and the window are flat, whichever way they face.
    REQUIRE(wall.vertices[0].color.r == 255);
    REQUIRE(window.vertices[0].color.r == 255);
    REQUIRE(f.device.draws[4].vertices[0].position == Vec3{10.0f, 0.0f, 100.0f});
    REQUIRE(f.device.draws[5].vertices[0].position == Vec3{10.0f, 0.0f, 60.0f});
    const auto& flame = f.device.draws[6];
    REQUIRE(flame.vertices[0].position == Vec3{10.0f, 0.0f, 30.0f});
    REQUIRE(flame.texture == &f.lender.texture(f.device, 0)); // lent by name
    REQUIRE(flame.blend() == BlendMode::Additive);
    REQUIRE_FALSE(flame.state.depthWrite);
    REQUIRE(flame.state.alphaTest == WorldScene::kAlphaTest);
    REQUIRE(flame.vertices[0].color.r == 255); // added whole, however it faces the light
    const auto& glow = f.device.draws[7];
    REQUIRE(glow.blend() == BlendMode::Additive);
    REQUIRE(glow.vertices[0].position == Vec3{12.0f, 0.0f, 0.0f});
    REQUIRE(glow.texture == wall.texture);
    REQUIRE(glow.vertices[0].color.g == 255);

    // Seen from beyond the far pane, the sorted units come the other way round.
    f.device.draws.clear();
    CameraFrame reverse = CameraFrame::at(Vec3{10.0f, 0.0f, 200.0f});
    reverse.forward = Vec3{0, 0, -1};
    f.scene.draw(f.device, Mat4{1.0f}, reverse);
    REQUIRE(f.device.draws[4].vertices[0].position == Vec3{10.0f, 0.0f, 30.0f});
    REQUIRE(f.device.draws[6].vertices[0].position == Vec3{10.0f, 0.0f, 100.0f});

    f.scene.clear();
    REQUIRE_FALSE(f.scene.built());
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1.0f});
    REQUIRE(f.device.draws.empty());
}

TEST_CASE("deferred depth biases layer light sheets after curtains without disabling depth",
          "[world][scene][temple-occlusion]") {
    Fixture f("world-scene-depth-bias");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"PANE","position":[100,0,5],"next":1,"child":-1,"objectFlags":2048},
      {"name":"PANE","position":[0,0,20],"next":2,"child":-1,"objectFlags":2048},
      {"name":"FLAME","position":[0,0,40],"next":3,"child":-1,"objectFlags":13113472},
      {"name":"FLAME","position":[0,0,50],"next":-1,"child":-1,"objectFlags":8915072}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    f.scene.drawDeferred(f.device, Mat4{1}, CameraFrame{});
    REQUIRE(f.device.draws.size() == 4);
    // Sideways distance must not outrank view depth. Of the biased sheets, the one
    // with both bits uses -20000, not their sum, and follows the -10000 sheet.
    CHECK(f.device.draws[0].vertices.front().position.z == 20);
    CHECK(f.device.draws[1].vertices.front().position.z == 5);
    CHECK(f.device.draws[2].vertices.front().position.z == 50);
    CHECK(f.device.draws[3].vertices.front().position.z == 40);
    for (const auto& draw : f.device.draws) {
        CHECK(draw.state.depthTest);
    }
    CHECK_FALSE(f.device.draws[3].state.depthWrite);
}

TEST_CASE("camera-depth scenery fades near the eye without changing gameplay opacity",
          "[world][scene][camera-depth-fade]") {
    Fixture f("world-camera-depth-fade");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[0,0,50],"next":1,"child":-1,"objectFlags":1024},
      {"name":"WALL","position":[1000,0,50],"next":-1,"child":-1,"objectFlags":1048576}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    for (const f32 eye : {0.0f, -200.0f, 100.0f, 0.0f}) {
        f.device.draws.clear();
        const auto camera = CameraFrame::at(Vec3{0, 0, eye});
        f.scene.drawOpaque(f.device, Mat4{1}, camera);
        if (eye == 0) {
            CHECK(f.device.draws.empty()); // no sort bit is needed for the near-distance queue
        }
        f.scene.drawDeferred(f.device, Mat4{1}, camera);
        if (eye == 100) {
            CHECK(f.device.draws.empty());
        } else {
            REQUIRE(f.device.draws.size() == 2);
            for (const auto& draw : f.device.draws) {
                const bool longFade = draw.vertices.front().position.x > 100;
                const u8 expected = eye < 0 ? 255 : (longFade ? 63 : 127);
                CHECK(draw.vertices.front().color.a == expected);
                CHECK(draw.state.depthWrite); // camera fade keeps the authored depth mask
            }
        }
        CHECK(f.scene.objectAlpha(0) == 1);
        CHECK(f.scene.objectAlpha(1) == 1);
    }
}

TEST_CASE("Temple arches columns and slabs use their authored camera fade",
          "[world][scene][camera-depth-fade][assets]") {
    const auto root = test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout native;
    REQUIRE(models.load(root));
    REQUIRE(textures.load(root));
    REQUIRE(native.load(root));
    const auto directory = test::scratchDirectory("temple-camera-fade");
    usize checked = 0;
    for (const auto& object : native.objects()) {
        if ((object.objectFlags & object_flags::kDistanceFade) == 0) {
            continue;
        }
        CAPTURE(object.name);
        if (!models.find(object.name)) {
            // The native roster also carries a meshless group and two names
            // without geometry in this archive; they submit no world draw.
            CHECK((object.name == "E1XD_BIGSLAB01" || object.name == "E1XD_BLOCKPI#0" ||
                   object.name == "E1XD_BLOCKPI#1"));
            continue;
        }
        // Draw each real mesh in isolation at its origin, using its native material flags.
        writeTextFile(directory / "world.json",
                      R"({"objects":[{"name":")" + object.name +
                          R"(","position":[0,0,0],"next":-1,"child":-1,"objectFlags":)" +
                          std::to_string(object.objectFlags) + "}]}");
        WorldLayout layout;
        WorldScene scene;
        REQUIRE(layout.load(directory));
        REQUIRE(scene.build(layout, models, textures, device));
        REQUIRE(scene.unitCount() == 1);
        device.draws.clear();
        scene.drawOpaque(device, Mat4{1}, CameraFrame::at({0, 0, -50}));
        CHECK(device.draws.empty());
        scene.drawDeferred(device, Mat4{1}, CameraFrame::at({0, 0, -50}));
        REQUIRE_FALSE(device.draws.empty());
        for (const auto& draw : device.draws) {
            CHECK(draw.state.depthWrite);
            CHECK(draw.vertices.front().color.a == 127);
        }
        CHECK(scene.objectAlpha(0) == 1);
        ++checked;
    }
    CHECK(checked == 19);
}

TEST_CASE("near-camera fading keeps AddDistObject's authored layer scales",
          "[world][scene][camera-depth-fade]") {
    Fixture f("world-camera-fade-layers");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[0,0,5],"next":1,"child":-1,"objectFlags":1024},
      {"name":"WALL","position":[0,0,50],"next":2,"child":-1,"objectFlags":525312},
      {"name":"WALL","position":[0,0,40],"next":-1,"child":-1,"objectFlags":4719744}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    f.scene.drawOpaque(f.device, Mat4{1}, CameraFrame{});
    CHECK(f.device.draws.empty());
    f.scene.drawDeferred(f.device, Mat4{1}, CameraFrame{});
    REQUIRE(f.device.draws.size() == 3);
    // AddDistObject scales instead of applying AddSortObject's constant bias.
    // Both flags select 0x400000's scale rather than multiplying the two.
    CHECK(f.device.draws[0].vertices.front().position.z == 5);
    CHECK(f.device.draws[1].vertices.front().position.z == 50);
    CHECK(f.device.draws[2].vertices.front().position.z == 40);
    CHECK(f.device.draws[0].state.depthWrite);
    CHECK(f.device.draws[1].state.depthWrite);
    CHECK_FALSE(f.device.draws[2].state.depthWrite); // explicitly authored 0x80
}

TEST_CASE("Temple broken column fades its shaft but keeps its native cap and pedestal solid",
          "[world][scene][camera-depth-fade][temple-column][assets]") {
    const auto root = test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout native;
    REQUIRE(models.load(root));
    REQUIRE(textures.load(root));
    REQUIRE(native.load(root));
    const auto directory = test::scratchDirectory("temple-column-parts");
    for (const usize index : {189U, 190U, 196U}) {
        const auto& object = native.objects().at(index);
        const bool shaft = index == 190;
        CAPTURE(object.name);
        REQUIRE(object.name == (index == 189 ? "E1COLUMN#05"
                                : shaft      ? "E1NSXD_COLUM#0"
                                             : "E1COLUMN#07"));
        REQUIRE(object.objectFlags == (shaft ? 0x1400U : 0U));
        REQUIRE(object.parent == -1);
        // NewWorldObject (800AB780) applies each piece's flags independently;
        // the broken top and pedestal do not inherit the adjacent shaft's fade.
        writeTextFile(directory / "world.json",
                      R"({"objects":[{"name":")" + object.name +
                          R"(","position":[0,0,0],"next":-1,"child":-1,"flags":2,"objectFlags":)" +
                          std::to_string(object.objectFlags) + "}]}");
        WorldLayout layout;
        WorldScene scene;
        REQUIRE(layout.load(directory));
        REQUIRE(scene.build(layout, models, textures, device));
        device.draws.clear();
        scene.drawOpaque(device, Mat4{1}, CameraFrame::at({0, 0, -50}));
        CHECK(device.draws.empty() == shaft);
        scene.drawDeferred(device, Mat4{1}, CameraFrame::at({0, 0, -50}));
        REQUIRE_FALSE(device.draws.empty());
        for (const auto& draw : device.draws) {
            CHECK(draw.state.depthWrite);
            for (const auto& vertex : draw.vertices) {
                CHECK(vertex.color.a == (shaft ? 127 : 255));
            }
        }
    }
}

TEST_CASE("camera fade overrides affect only selected geometry and never simulation state",
          "[world][scene][camera-depth-fade][camera-fade-override]") {
    Fixture f("world-camera-fade-override");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[0,0,50],"next":1,"child":-1,"objectFlags":0},
      {"name":"WALL","position":[10,0,50],"next":2,"child":-1,"objectFlags":0},
      {"name":"WALL","position":[20,0,50],"next":-1,"child":-1,"objectFlags":1024}]})");
    REQUIRE(f.layout.load(f.directory));
    const std::array<usize, 1> overrides{0};
    REQUIRE(f.build({}, {}, overrides));
    CHECK(f.layout.objects()[0].objectFlags == 0);
    CHECK(f.layout.objects()[1].objectFlags == 0);
    CHECK(f.layout.objects()[2].objectFlags == 1024);
    DigestTextures textures(f);
    const auto before = sceneDigest(f.scene, textures.resources());
    REQUIRE(before);
    for (const f32 eye : {0.0f, -200.0f, 0.0f}) {
        f.device.draws.clear();
        f.scene.draw(f.device, Mat4{1}, CameraFrame::at({0, 0, eye}));
        REQUIRE(f.device.draws.size() == 3);
        for (const auto& draw : f.device.draws) {
            const bool unchangedSolid = draw.vertices.front().position.x == 10;
            CHECK(draw.vertices.front().color.a == (unchangedSolid || eye < 0 ? 255 : 127));
        }
        CHECK(f.scene.objectAlpha(0) == 1);
        CHECK(f.scene.objectAlpha(2) == 1);
        CHECK(sceneDigest(f.scene, textures.resources()) == before);
    }
    REQUIRE(f.build()); // the override cannot leak into a subsequent world build
    f.device.draws.clear();
    f.scene.drawOpaque(f.device, Mat4{1}, CameraFrame{});
    REQUIRE(f.device.draws.size() == 1);
    CHECK(f.device.draws.front().vertices.size() == 6); // both unflagged walls are solid again
}

TEST_CASE("Temple stained-glass light rays composite after the curtain meshes",
          "[world][scene][temple-occlusion][assets]") {
    const auto root = test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    REQUIRE(models.load(root));
    REQUIRE(textures.load(root));
    REQUIRE(layout.load(root));
    REQUIRE(scene.build(layout, models, textures, device));
    const auto* rays = scene.textureOf(54);     // LIGHTRAY
    const auto* curtains = scene.textureOf(84); // REDBACKCURTAIN2
    REQUIRE(rays);
    REQUIRE(curtains);
    REQUIRE(rays != curtains);
    scene.draw(device, Mat4{1}, CameraFrame{});
    usize firstRay = device.draws.size();
    usize lastCurtain = 0;
    usize curtainCount = 0;
    for (usize i = 0; i < device.draws.size(); ++i) {
        const auto& draw = device.draws[i];
        if (draw.texture == rays) {
            firstRay = std::min(firstRay, i);
            CHECK(draw.state.depthTest);
            CHECK_FALSE(draw.state.depthWrite);
        }
        if (draw.texture == curtains) {
            lastCurtain = i;
            ++curtainCount;
        }
    }
    REQUIRE(curtainCount > 0);
    REQUIRE(firstRay < device.draws.size());
    CHECK(lastCurtain < firstRay);
}

TEST_CASE("alpha-textured scenery is a solid unless its object is authored to sort",
          "[world][scene][draw-policy]") {
    // MBSetupObject defers only sortable objects (0x800) that are faded, glowing, flagged
    // behind, or whose mesh is marked alpha-textured; the texture alone decides nothing
    // but when the solid is drawn: one wearing a translucent texture is a veil, drawn with
    // the solids' depth rules after the figures standing under it.
    Fixture f("world-scene-draw-policy");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[0,0,0],"next":1,"child":-1,"objectFlags":0},
      {"name":"PANE","position":[0,0,10],"next":2,"child":-1,"objectFlags":0},
      {"name":"PANE","position":[0,0,20],"next":3,"child":-1,"objectFlags":2048},
      {"name":"WALL","position":[0,0,30],"next":-1,"child":-1,"objectFlags":2048}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    REQUIRE(f.scene.batchCount() == 2); // the wall and the unsorted pane, both solids
    REQUIRE(f.scene.unitCount() == 2);  // the sortable pane and the sortable wall
    const Texture* stone = &f.textures.texture(f.device, 0);
    const Texture* glass = &f.textures.texture(f.device, 1);
    const CameraFrame camera;

    f.scene.drawOpaque(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 2);
    CHECK(f.device.draws[0].texture == stone);
    const auto& sortableWall = f.device.draws[1];
    CHECK(sortableWall.texture == stone);
    CHECK(sortableWall.vertices[0].position.z == 30.0f); // opaque mesh: solid until faded
    CHECK(sortableWall.state.depthWrite);

    f.device.draws.clear();
    f.scene.drawVeils(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 1);
    const auto& pane = f.device.draws[0];
    CHECK(pane.texture == glass);
    CHECK(pane.vertices[0].position.z == 10.0f);
    CHECK(pane.state.depthWrite);
    CHECK(pane.state.depthTest);
    CHECK(pane.state.alphaTest == WorldScene::kAlphaTest);

    f.device.draws.clear();
    f.scene.drawDeferred(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 1);
    CHECK(f.device.draws[0].texture == glass);
    CHECK(f.device.draws[0].vertices[0].position.z == 20.0f);

    // Fading the sortable wall is what moves it into the deferred queue.
    f.scene.setObjectAlpha(3, 0.5f);
    f.device.draws.clear();
    f.scene.drawOpaque(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 1);
    f.scene.drawVeils(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 2);
    f.scene.drawDeferred(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 4);
    CHECK(f.device.draws[2].vertices[0].position.z == 30.0f); // farther first
    CHECK_FALSE(f.device.draws[2].state.depthWrite);
    CHECK(f.device.draws[3].vertices[0].position.z == 20.0f);

    // Drawn as one, the scene goes solids, veils, then the sorted objects.
    f.scene.setObjectAlpha(3, 1.0f);
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 4);
    CHECK(f.device.draws[0].texture == stone);
    CHECK(f.device.draws[1].vertices[0].position.z == 30.0f);
    CHECK(f.device.draws[2].vertices[0].position.z == 10.0f);
    CHECK(f.device.draws[3].vertices[0].position.z == 20.0f);
}

TEST_CASE("the Castle Treasury weapon rack is a veil drawn with the solids' depth rules",
          "[world][scene][assets]") {
    const auto root = test::assetOrSkip("LEVELS/LEVELA4/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    REQUIRE(models.load(root));
    REQUIRE(textures.load(root));
    REQUIRE(layout.load(root));
    // A4RECTANGLE192: a cutout quad on the wall, alpha-textured, with no sort flag.
    const auto rack = models.find("A4RECTANGLE192");
    REQUIRE(rack);
    REQUIRE((models.entry(*rack).flags & 1U) != 0);
    REQUIRE(models.mesh(*rack).alphaTextured());
    const auto slot = textures.find("SWRDSHLD_3");
    REQUIRE(slot);
    REQUIRE(textures.entry(*slot).translucent());
    const Texture* texture = &textures.texture(device, *slot);
    REQUIRE(scene.build(layout, models, textures, device, {}, {}));
    scene.drawOpaque(device, Mat4{1}, CameraFrame{});
    for (const auto& draw : device.draws) {
        CHECK(draw.texture != texture);
    }
    device.draws.clear();
    scene.drawVeils(device, Mat4{1}, CameraFrame{});
    usize veilDraws = 0;
    for (const auto& draw : device.draws) {
        if (draw.texture == texture) {
            ++veilDraws;
            CHECK(draw.state.depthWrite);
            CHECK(draw.state.depthTest);
            CHECK(draw.state.alphaTest == WorldScene::kAlphaTest);
        }
    }
    REQUIRE(veilDraws > 0);
    device.draws.clear();
    scene.drawDeferred(device, Mat4{1}, CameraFrame{});
    for (const auto& draw : device.draws) {
        CHECK(draw.texture != texture);
    }
}

TEST_CASE("the Trenches' haze is a veil: a solid drawn after the figures standing under it",
          "[world][scene][veil][assets]") {
    const auto root = test::assetOrSkip("LEVELS/LEVELH1/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    REQUIRE(models.load(root));
    REQUIRE(textures.load(root));
    REQUIRE(layout.load(root));
    // H1#37: the level-wide sheet of BATTLEHAZE, alpha-textured, with no sort flag, that
    // the original draws before its items and so cuts a generator off behind.
    const auto slot = textures.find("BATTLEHAZE");
    REQUIRE(slot);
    REQUIRE(textures.entry(*slot).translucent());
    const Texture* haze = &textures.texture(device, *slot);
    REQUIRE(scene.build(layout, models, textures, device, {}, {}));
    const CameraFrame camera = CameraFrame::at(Vec3{68.0f, 11.0f, 304.0f});
    scene.drawOpaque(device, Mat4{1}, camera);
    REQUIRE_FALSE(device.draws.empty());
    for (const auto& draw : device.draws) {
        CHECK(draw.texture != haze);
    }
    device.draws.clear();
    scene.drawVeils(device, Mat4{1}, camera);
    usize hazeDraws = 0;
    for (const auto& draw : device.draws) {
        if (draw.texture == haze) {
            ++hazeDraws;
            CHECK(draw.state.depthWrite);
            CHECK(draw.state.depthTest);
            CHECK(draw.state.alphaTest == WorldScene::kAlphaTest);
            CHECK(draw.blend() == BlendMode::Alpha);
        }
    }
    REQUIRE(hazeDraws > 0);
    device.draws.clear();
    scene.drawDeferred(device, Mat4{1}, camera);
    for (const auto& draw : device.draws) {
        CHECK(draw.texture != haze);
    }
}

TEST_CASE("S8 wheat cutouts reject clear texels in both batched and independent scenery",
          "[world][scene][wheat-alpha][assets]") {
    const auto root = test::assetOrSkip("LEVELS/LEVELS8/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    REQUIRE(models.load(root));
    REQUIRE(textures.load(root));
    REQUIRE(layout.load(root));
    const auto wheat = textures.find("WHEAT_STANDING_TOP2");
    REQUIRE(wheat);
    REQUIRE_FALSE(textures.entry(*wheat).translucent());
    const auto& texture = dynamic_cast<const test::FakeTexture&>(textures.texture(device, *wheat));
    usize clear = 0;
    usize solid = 0;
    for (usize i = 3; i < texture.pixels.size(); i += 4) {
        clear += texture.pixels[i] == 0 ? 1 : 0;
        solid += texture.pixels[i] == 255 ? 1 : 0;
    }
    REQUIRE(clear == 4357);
    REQUIRE(solid == 3835);
    std::vector<usize> controlled;
    for (const bool individual : {false, true}) {
        INFO(individual);
        if (individual) {
            for (usize i = 0; i < layout.objects().size(); ++i) {
                controlled.push_back(i);
            }
        }
        REQUIRE(scene.build(layout, models, textures, device, {}, {}, controlled));
        device.draws.clear();
        scene.drawOpaque(device, Mat4{1}, CameraFrame{});
        usize wheatDraws = 0;
        for (const auto& draw : device.draws) {
            if (draw.texture == &texture) {
                ++wheatDraws;
                CHECK(draw.state.depthWrite);
                CHECK(draw.state.depthTest);
                CHECK(draw.state.alphaTest == WorldScene::kAlphaTest);
                CHECK(draw.state.usesAlphaToCoverage(4));
            }
        }
        REQUIRE(wheatDraws > 0); // Binary-alpha art remains in the solid pass.
    }
}

TEST_CASE("world depth comparison flags survive batching and independent unit draws",
          "[world][scene][vfx-depth]") {
    // A camera-facing glow keeps its authored no-compare word (the facing mode is the top
    // nibble); a wall that only leaves depth unwritten stays batched.
    Fixture f("world-scene-depth-compare");
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[0,0,0],"next":1,"objectFlags":0},
      {"name":"WALL","position":[10,0,0],"next":2,"objectFlags":16777280},
      {"name":"WALL","position":[20,0,0],"next":3,"objectFlags":128},
      {"name":"PANE","position":[30,0,0],"next":4,"objectFlags":2048},
      {"name":"PANE","position":[40,0,0],"next":5,"objectFlags":16779328},
      {"name":"PANE","position":[50,0,0],"next":-1,"objectFlags":16779456}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    REQUIRE(f.scene.batchCount() == 2);
    REQUIRE(f.scene.unitCount() == 4);
    f.scene.draw(f.device, Mat4{1}, CameraFrame{});
    REQUIRE(f.device.draws.size() == 6);
    for (const auto& draw : f.device.draws) {
        const auto placement = static_cast<s32>(std::round(draw.vertices.front().position.x / 10));
        INFO(placement);
        const bool tested = placement == 0 || placement == 2 || placement == 3;
        const bool written = placement != 2 && placement != 5;
        CHECK(draw.state.depthTest == tested);
        CHECK(draw.state.depthWrite == written);
        CHECK(draw.state.usesAlphaToCoverage(4) == (tested && written));
    }
}

TEST_CASE("world transparency can be deferred until dynamic solid objects have drawn",
          "[world][scene]") {
    Fixture f("world-scene-deferred");
    REQUIRE(f.build());
    const CameraFrame camera;
    f.scene.draw(f.device, Mat4{1}, camera);
    const auto together = f.device.draws;
    f.device.draws.clear();
    f.scene.drawOpaque(f.device, Mat4{1}, camera);
    const auto solidCount = f.device.draws.size();
    REQUIRE(solidCount > 0);
    REQUIRE(solidCount < together.size());
    f.scene.drawVeils(f.device, Mat4{1}, camera);
    f.scene.drawDeferred(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == together.size());
    for (usize i = 0; i < together.size(); ++i) {
        CHECK(f.device.draws[i].vertices.size() == together[i].vertices.size());
        CHECK(f.device.draws[i].vertices.front().position == together[i].vertices.front().position);
    }
}

TEST_CASE("depthless background lightning precedes solids and is not drawn again over actors",
          "[world][scene][chimera]") {
    Fixture f("world-scene-background");
    // A5's 0xc81880: sorted, sort-behind, additive, no depth write. The sheet's
    // vertices can be nearer than the arena: its authored layer takes precedence.
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[0,0,30],"next":1,"child":-1},
      {"name":"FLAME","position":[0,0,1],"next":2,"child":-1,"objectFlags":13113472},
      {"name":"FLAME","position":[0,0,2],"next":-1,"child":-1,"objectFlags":8390784}]})");
    REQUIRE(f.layout.load(f.directory));
    const std::array<usize, 1> background{1};
    REQUIRE(f.build({}, background));
    const CameraFrame camera;
    f.scene.drawOpaque(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 2);
    CHECK(f.device.draws[0].vertices.front().position.z == 1);
    CHECK_FALSE(f.device.draws[0].state.depthWrite);
    CHECK(f.device.draws[1].vertices.front().position.z == 30);
    f.device.draws.clear(); // actors are drawn here by PlayScene
    f.scene.drawDeferred(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 1);
    CHECK(f.device.draws[0].vertices.front().position.z == 2); // ordinary flames still deferred
}

TEST_CASE("sort-behind forcefields blend after solids instead of disappearing beneath them",
          "[world][scene][forcefield]") {
    Fixture f("world-scene-forcefield");
    // L1's forcefield has 0xc01880. Both sorting bits together are also only
    // queue biases unless the level explicitly requests a backdrop.
    writeTextFile(f.directory / "world.json", R"({"objects":[
      {"name":"WALL","position":[0,0,30],"next":1,"child":-1},
      {"name":"FLAME","position":[0,0,1],"next":2,"child":-1,"objectFlags":12589184},
      {"name":"FLAME","position":[0,0,2],"next":-1,"child":-1,"objectFlags":13113472}]})");
    REQUIRE(f.layout.load(f.directory));
    REQUIRE(f.build());
    const CameraFrame camera;
    f.scene.drawOpaque(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 1);
    CHECK(f.device.draws.front().vertices.front().position.z == 30);
    f.device.draws.clear();
    f.scene.drawDeferred(f.device, Mat4{1}, camera);
    REQUIRE(f.device.draws.size() == 2);
    for (const auto& draw : f.device.draws) {
        CHECK(draw.blend() == BlendMode::Additive);
        CHECK_FALSE(draw.state.depthWrite);
    }
    f.scene.setObjectAlpha(1, 0);
    f.scene.setObjectAlpha(2, 0);
    f.device.draws.clear();
    f.scene.drawDeferred(f.device, Mat4{1}, camera);
    CHECK(f.device.draws.empty());
}

TEST_CASE("moving objects carry what stands under them", "[world][scene]") {
    Fixture f("world-scene-moving");
    REQUIRE(f.build());
    // A quarter turn of the spinning group swings its blade from +x to -z.
    const Mat4 turned = glm::rotate(glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.0f, 50.0f}), kHalfPi,
                                    Vec3{0.0f, 1.0f, 0.0f});
    f.scene.setObjectTransform(8, turned);
    f.scene.setObjectTransform(2, turned); // the still wall ignores it
    f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 0.0f});
    const auto& blade = f.device.draws[2];
    REQUIRE(blade.vertices[0].position.x == Approx(10.0f).margin(1e-5f));
    REQUIRE(blade.vertices[0].position.z == Approx(49.0f).margin(1e-5f));
    REQUIRE(blade.vertices[1].position.z == Approx(48.0f).margin(1e-5f));
    REQUIRE(f.device.draws[0].vertices[0].position == Vec3{11.0f, 0.0f, 0.0f});
}

TEST_CASE("texture slots can show another frame or slide their coordinates", "[world][scene]") {
    Fixture f("world-scene-slots");
    REQUIRE(f.build());
    REQUIRE(f.scene.textureOf(1) == &f.textures.texture(f.device, 1));
    REQUIRE(f.scene.textureOf(3) == &f.lender.texture(f.device, 0));
    REQUIRE(f.scene.textureOf(99) == nullptr);
    const Texture& frame = f.lender.texture(f.device, 1);
    f.scene.setTextureFrame(0, &frame);
    f.scene.setTextureOffset(1, Vec2{0.25f, 0.0f});
    REQUIRE(f.scene.textureOf(0) == &frame);
    REQUIRE(f.scene.textureOffset(1) == Vec2{0.25f, 0.0f});
    f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 0.0f});
    REQUIRE(f.device.draws[0].texture == &frame); // the wall
    REQUIRE(f.device.draws[2].texture == &frame); // and the blade, in the same slot
    REQUIRE(f.device.draws[3].state.uvOffset == Vec2{0.25f, 0.0f}); // the window, a veil
    REQUIRE(f.device.draws[0].state.uvOffset == Vec2{0.0f, 0.0f});
    f.scene.setTextureFrame(0, nullptr);
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 0));
    // Slots the scene never draws are ignored.
    f.scene.setTextureFrame(99, &frame);
    f.scene.setTextureOffset(99, Vec2{1.0f, 1.0f});
    REQUIRE(f.scene.textureOffset(99) == Vec2{0.0f, 0.0f});
}

TEST_CASE("world flipbooks blend soft sheets but preserve solid and lightmapped coverage",
          "[world][scene][texture-blend]") {
    Fixture f("world-scene-flipbooks");
    REQUIRE(f.build());
    const std::array<const Texture*, 2> frames{&f.textures.texture(f.device, 0),
                                               &f.textures.texture(f.device, 1)};
    f.scene.setTextureCycle(0, frames, 0, 1);
    f.scene.setTextureCycle(3, frames, 0, 1);
    f.scene.draw(f.device, Mat4{1}, CameraFrame{}, 0.5f, 0.5f);
    REQUIRE(f.device.draws.size() == 8);
    for (const auto& draw : f.device.draws) {
        if (draw.state.blend == BlendMode::Additive) {
            CHECK(draw.texture == frames[0]);
            CHECK(draw.state.nextTexture == frames[1]);
            CHECK(draw.state.textureBlend == Approx(0.5f));
        } else {
            CHECK(draw.state.nextTexture == nullptr);
        }
    }
    // A temporary alpha fade is not evidence that an opaque mesh is a soft sprite.
    f.scene.setObjectAlpha(10, 0.5f);
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1}, CameraFrame{}, 0.5f, 0.5f);
    for (const auto& draw : f.device.draws) {
        if (draw.state.blend != BlendMode::Additive) {
            CHECK(draw.state.nextTexture == nullptr);
        }
    }
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1});
    for (const auto& draw : f.device.draws) {
        CHECK(draw.state.nextTexture == nullptr);
    }
}

TEST_CASE("objects flagged to face the camera are units turned its way", "[world][scene]") {
    Fixture f("world-scene-facing");
    // The near pane asks to face the camera about the vertical.
    const auto dir = test::scratchDirectory("world-scene-facing-layout");
    writeTextFile(dir / "world.json", R"({"objects": [
      {"name": "WALL", "position": [0, 0, 0], "next": 1},
      {"name": "PANE", "position": [0, 0, 10], "next": -1, "objectFlags": 16777216}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    REQUIRE(f.scene.build(layout, f.models, f.textures, f.device));
    REQUIRE(f.scene.unitCount() == 1);
    const CameraFrame camera = CameraFrame::at(Vec3{10.0f, 0.0f, 10.0f});
    f.scene.draw(f.device, Mat4{1.0f}, camera);
    REQUIRE(f.device.draws.size() == 2);
    // The pane's x edge swings to -z with its z axis pointing at the camera.
    const Vec3 edge = f.device.draws[1].vertices[1].position;
    REQUIRE(edge.x == Approx(0.0f).margin(1e-5f));
    REQUIRE(edge.z == Approx(9.0f).margin(1e-5f));
}

TEST_CASE("a mesh with a colour stream is shaded by its vertices whatever its flags",
          "[world][scene]") {
    Fixture f("world-scene-prelit");
    // Two copies of the same dim wall, one carrying the flag the original never reads: both
    // show their baked colour, never the level's light (K1's floors lack the flag and are
    // baked all the same).
    const auto dir = test::scratchDirectory("world-scene-prelit-layout");
    writeTextFile(dir / "world.json", R"({"objects": [
      {"name": "LIT", "position": [0, 0, 0], "next": 1, "flags": 2},
      {"name": "LIT", "position": [0, 0, 10], "next": -1, "flags": 0}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    REQUIRE(f.scene.build(layout, f.models, f.textures, f.device));
    f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 10.0f});
    REQUIRE(f.device.draws.size() == 1); // both share one batch
    const auto& vertices = f.device.draws[0].vertices;
    REQUIRE(vertices.size() == 6);
    for (const ImmediateVertex& v : vertices) {
        // The fixture's 0.2, quantized to five bits (48) and spread so 248 is the whole.
        CHECK(v.color.r == 49);
    }
}

TEST_CASE("a baked five-bit colour reaches the whole texture at 248", "[world][scene]") {
    CHECK(WorldScene::bakedShade(Color::rgba(248, 124, 0, 200)) == Color::rgba(255, 128, 0, 200));
    CHECK(WorldScene::bakedShade(Color::rgba(48, 48, 48)) == Color::rgba(49, 49, 49));
    CHECK(WorldScene::bakedShade(Color::rgba(255, 255, 255)) == Color::rgba(255, 255, 255));
}

TEST_CASE("world chrome coordinates stay finite for absent and collapsed mesh normals",
          "[world][world-chrome][world-local-rewind]") {
    const bool zeroNormal = GENERATE(false, true);
    const bool collapsed = GENERATE(false, true);
    const auto directory = test::sampleLevel("world-chrome-zero-normal");
    writeTextFile(directory / "models/006_LIT.obj",
                  std::string{"v 0 0 0 0.2 0.2 0.2\nv 1 0 0 0.2 0.2 0.2\n"
                              "v 0 1 0 0.2 0.2 0.2\n"} +
                      (zeroNormal ? "vn 0 0 0\n" : "vn 0 1 0\n") +
                      "usemtl tex0\nf 1//1 2//1 3//1\n");
    test::convertModelFixture(directory);
    writeTextFile(directory / "world.json", R"({"objects":[
      {"name":"LIT","position":[0,0,0],"next":-1,"child":-1,
       "flags":4098,"objectFlags":32768}]})");
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    REQUIRE(layout.load(directory));
    REQUIRE(scene.build(layout, models, textures, device));
    REQUIRE(scene.moving(0));
    if (collapsed) {
        scene.setObjectTransform(0, glm::scale(Mat4{1}, Vec3{0}), true);
    }
    scene.draw(device, Mat4{1});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws.front().vertices.size() == 3);
    const auto index = models.find("LIT");
    REQUIRE(index);
    const auto& mesh = models.mesh(*index);
    REQUIRE(mesh.prelit);
    for (const auto& vertex : device.draws.front().vertices) {
        CHECK(std::isfinite(vertex.uv.x));
        CHECK(std::isfinite(vertex.uv.y));
        CHECK(vertex.color == WorldScene::bakedShade(mesh.vertices.front().color));
        CHECK(vertex.uv == (zeroNormal || collapsed ? Vec2{0.5f} : Vec2{0.5f, 0}));
    }
}

TEST_CASE("Ghost Town world chrome draws contain finite texture coordinates",
          "[assets][world-chrome][world-local-rewind]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELG2/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    REQUIRE(layout.load(directory));
    REQUIRE(scene.build(layout, models, textures, device));
    scene.draw(device, Mat4{1}, CameraFrame::at(Vec3{0, 20, 0}), 0.5f);
    REQUIRE_FALSE(device.draws.empty());
    for (usize draw = 0; draw < device.draws.size(); ++draw) {
        const auto& vertices = device.draws[draw].vertices;
        for (usize vertex = 0; vertex < vertices.size(); ++vertex) {
            if (!std::isfinite(vertices[vertex].uv.x) || !std::isfinite(vertices[vertex].uv.y)) {
                CAPTURE(draw, vertex);
                FAIL("non-finite world texture coordinate");
            }
        }
    }
}

TEST_CASE("an object flagged to go without its lightmap is drawn flat", "[world][scene]") {
    Fixture f("world-scene-flat");
    const auto dir = test::scratchDirectory("world-scene-flat-layout");
    writeTextFile(dir / "world.json", R"({"objects": [
      {"name": "FLOOR", "position": [0, 0, 0], "next": 1},
      {"name": "FLOOR", "position": [0, 0, 40], "next": -1, "objectFlags": 16384}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    REQUIRE(f.scene.build(layout, f.models, f.textures, f.device));
    f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 10.0f});
    REQUIRE(f.device.draws.size() == 2); // the lightmapped floor and the flat one apart
    const bool firstMapped = f.device.draws[0].lightmap() != nullptr;
    const auto& mapped = f.device.draws[firstMapped ? 0 : 1];
    const auto& flat = f.device.draws[firstMapped ? 1 : 0];
    CHECK(mapped.lightmap() == &f.textures.texture(f.device, 2));
    CHECK(flat.lightmap() == nullptr);
    for (const ImmediateVertex& v : flat.vertices) {
        CHECK(v.color == Color::white());
    }
}

TEST_CASE("point lights brighten the baked geometry they reach, prelit or not",
          "[world][scene][lights]") {
    Fixture f("world-scene-lights");
    const auto dir = test::scratchDirectory("world-scene-lights-layout");
    writeTextFile(dir / "world.json", R"({"objects": [
      {"name": "LIT", "position": [0, 0, 0], "next": -1, "flags": 2}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    REQUIRE(f.scene.build(layout, f.models, f.textures, f.device));
    const auto reddest = [&] {
        f.device.draws.clear();
        f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 10.0f});
        REQUIRE(f.device.draws.size() == 1);
        u8 most = 0;
        for (const ImmediateVertex& v : f.device.draws[0].vertices) {
            most = std::max(most, v.color.r);
        }
        return most;
    };
    const u8 plain = reddest();
    REQUIRE(plain == 49); // its own baked colour, five bits spread so 248 is the whole
    // A red light on every side of it, whichever way it faces.
    std::vector<PointLight> lights;
    for (const Vec3 side : {Vec3{3, 0, 0}, Vec3{-3, 0, 0}, Vec3{0, 3, 0}, Vec3{0, -3, 0},
                            Vec3{0, 0, 3}, Vec3{0, 0, -3}}) {
        PointLight light;
        light.position = side;
        light.color = Vec3{2, 0, 0};
        light.radius = 20.0f;
        lights.push_back(light);
    }
    f.scene.setPointLights(lights);
    CHECK(reddest() > plain);
    // Darkened, the lit copy carries the darkening in its colours with the light over it,
    // while an unlit batch is darkened as it is drawn.
    f.scene.setDarken(0.5f);
    CHECK(reddest() > plain);
    CHECK(f.device.draws[0].state.darken == 0.0f);
    f.scene.setPointLights({});
    CHECK(reddest() == plain);
    CHECK(f.device.draws[0].state.darken == 0.5f);
    f.scene.setDarken(0.0f);
    CHECK(reddest() == plain);
}

TEST_CASE("moving world geometry interpolates its local hierarchy without moving collision",
          "[world][scene][presentation]") {
    Fixture f("world-scene-presentation");
    REQUIRE(f.build());
    f.scene.capturePresentation();
    const Mat4 current = glm::rotate(glm::translate(Mat4{1}, Vec3{6, 2, 50}), 1.0f, Vec3{0, 1, 0});
    f.scene.setObjectTransform(8, current);
    const Mat4 native = f.scene.worldTransform(10);
    for (const f32 alpha : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.5f}) {
        f.device.draws.clear();
        f.scene.draw(f.device, Mat4{1}, CameraFrame::at(Vec3{0}), alpha);
        REQUIRE(f.device.draws.size() == 8);
        const Vec3 vertex = f.device.draws[2].vertices[0].position;
        CHECK(vertex.x == Approx(10 + 6 * alpha + std::cos(alpha)).margin(1e-5f));
        CHECK(vertex.y == Approx(2 * alpha));
        CHECK(vertex.z == Approx(50 - std::sin(alpha)).margin(1e-5f));
        CHECK(f.scene.worldTransform(10) == native);
    }
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1}, CameraFrame::at(Vec3{0}));
    CHECK(f.device.draws[2].vertices[0].position == Vec3{native * Vec4{1, 0, 0, 1}});

    // An explicit teleport starts at its destination even before the next snapshot.
    const Mat4 teleported = glm::translate(Mat4{1}, Vec3{80, 5, 50});
    f.scene.setObjectTransform(8, teleported, true);
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1}, CameraFrame::at(Vec3{0}), 0.0f);
    CHECK(f.device.draws[2].vertices[0].position == Vec3{91, 5, 50});
    f.scene.capturePresentation();
    for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
        f.device.draws.clear();
        f.scene.draw(f.device, Mat4{1}, CameraFrame::at(Vec3{0}), alpha);
        CHECK(f.device.draws[2].vertices[0].position == Vec3{91, 5, 50});
    }
}

TEST_CASE("an external texture nobody lends is drawn white", "[world][scene]") {
    Fixture f("world-scene-unlent");
    REQUIRE(f.scene.build(f.layout, f.models, f.textures, f.device));
    REQUIRE(f.scene.placedCount() == 8);
    REQUIRE(f.scene.textureOf(3) == &f.device.whiteTexture());
}

TEST_CASE("a layout whose objects have no meshes builds nothing", "[world][scene]") {
    Fixture f("world-scene-empty");
    const auto dir = test::scratchDirectory("world-scene-empty-layout");
    writeTextFile(dir / "world.json", R"({"objects": [{"name": "GHOST", "position": [0, 0, 0]}]})");
    WorldLayout ghosts;
    REQUIRE(ghosts.load(dir));
    REQUIRE_FALSE(f.scene.build(ghosts, f.models, f.textures, f.device));
    REQUIRE(f.scene.placedCount() == 0);
}

TEST_CASE("world immutable digest caches retain native resources and rebuild configuration",
          "[world][scene][native-immutable-digest][world-scene-digest]") {
    Fixture first("world-digest-first");
    Fixture second("world-digest-second");
    REQUIRE(first.build());
    REQUIRE(second.build());
    DigestTextures firstTextures(first);
    DigestTextures secondTextures(second);
    const auto initial = sceneDigest(first.scene, firstTextures.resources());
    REQUIRE(initial);
    REQUIRE(first.scene.textureOf(0) != second.scene.textureOf(0));
    CHECK(sceneDigest(second.scene, secondTextures.resources()) == initial);
    CHECK(sceneDigest(first.scene, firstTextures.resources()) == initial);
    WorldScene::LocalSnapshot saved;
    first.scene.saveLocal(saved);
    CHECK(sceneDigest(first.scene, firstTextures.resources(), &saved) == initial);
    CHECK_FALSE(sceneDigest(second.scene, secondTextures.resources(), &saved));

    // Immutable scalar caches never authenticate pointer bindings on behalf of
    // the current stage registry, even after a successful hash warmed them.
    CHECK_FALSE(sceneDigest(first.scene));
    auto different = firstTextures;
    different.slots[0].second += 1000;
    REQUIRE(sceneDigest(first.scene, different.resources()));
    CHECK(sceneDigest(first.scene, different.resources()) != initial);
    different = firstTextures;
    different.slots.erase(different.slots.begin());
    CHECK_FALSE(sceneDigest(first.scene, different.resources()));
    CHECK(sceneDigest(first.scene, firstTextures.resources(), &saved) == initial);

    for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
        first.device.draws.clear();
        first.scene.draw(first.device, Mat4{1}, CameraFrame{}, alpha);
        CHECK(sceneDigest(first.scene, firstTextures.resources()) == initial);
    }
    // Sample object 0 is a meshless parent, so controlling it changes nothing.
    // Object 2 is a baked WALL; control must create an additional mutable unit.
    const std::array<usize, 1> controlled{2};
    REQUIRE_FALSE(first.scene.moving(2));
    REQUIRE_FALSE(first.scene.setObjectVisible(2, true));
    const auto units = first.scene.unitCount();
    REQUIRE(first.build(controlled));
    REQUIRE(first.scene.unitCount() == units + 1);
    REQUIRE(first.scene.setObjectVisible(2, true));
    CHECK_FALSE(first.scene.canRestoreLocal(saved));
    CHECK_FALSE(sceneDigest(first.scene, firstTextures.resources(), &saved));
    CHECK(sceneDigest(first.scene, firstTextures.resources()) != initial);
    REQUIRE(first.build());
    CHECK(sceneDigest(first.scene, firstTextures.resources()) == initial);
    const std::array<usize, 1> backdrop{9};
    REQUIRE(first.build({}, backdrop));
    CHECK(sceneDigest(first.scene, firstTextures.resources()) != initial);
    REQUIRE(first.build());
    CHECK(sceneDigest(first.scene, firstTextures.resources()) == initial);

    first.scene.clear();
    const WorldScene empty;
    REQUIRE(sceneDigest(first.scene));
    CHECK(sceneDigest(first.scene) == sceneDigest(empty));
    CHECK(sceneDigest(first.scene) != initial);
    REQUIRE(first.build());
    CHECK(sceneDigest(first.scene, firstTextures.resources()) == initial);
}

TEST_CASE("world immutable caches do not hide mutable placement unit or texture state",
          "[world][scene][local-rewind][native-immutable-digest][world-scene-digest]") {
    Fixture fixture("world-digest-mutable");
    REQUIRE(fixture.build());
    DigestTextures textures(fixture);
    WorldScene::LocalSnapshot initialState;
    fixture.scene.saveLocal(initialState);
    const auto initial = sceneDigest(fixture.scene, textures.resources());
    REQUIRE(initial);
    const auto uploads = fixture.device.texturesCreated;
    const auto updates = fixture.device.textureUpdates;
    for (u32 field = 0; field < 10; ++field) {
        CAPTURE(field);
        REQUIRE(fixture.scene.restoreLocal(initialState));
        switch (field) {
        case 0: fixture.scene.setObjectTransform(8, glm::translate(Mat4{1}, Vec3{0, 8, 50})); break;
        case 1:
            fixture.scene.setObjectTransform(8, glm::translate(Mat4{1}, Vec3{0, 8, 50}), true);
            break;
        case 2: fixture.scene.setObjectAlpha(9, 0.75f); break;
        case 3: REQUIRE(fixture.scene.setObjectVisible(11, false)); break;
        case 4: fixture.scene.setTextureFrame(0, fixture.scene.textureOf(1)); break;
        case 5: {
            const std::array<const Texture*, 2> frames{fixture.scene.textureOf(0),
                                                       fixture.scene.textureOf(1)};
            fixture.scene.setTextureCycle(0, frames, 0.5f, 0.25f);
            break;
        }
        case 6:
            fixture.scene.setTextureScroll(0, fixture.scene.textureOffset(0), {0.25f, 0.5f},
                                           {0.01f, 0.02f});
            break;
        case 7: fixture.scene.setTextureTime(12); break;
        case 8: fixture.scene.setDarken(0.25f); break;
        case 9: {
            const std::array<PointLight, 1> lights{{{{0, 1, 50}, {1, 0.5f, 0.25f}, 4, 2}}};
            fixture.scene.setPointLights(lights);
            break;
        }
        default: FAIL("unexpected mutable field");
        }
        const auto changed = sceneDigest(fixture.scene, textures.resources());
        REQUIRE(changed);
        CHECK(changed != initial);
        WorldScene::LocalSnapshot changedState;
        fixture.scene.saveLocal(changedState);
        CHECK_FALSE(fixture.scene.equalLocal(initialState, changedState));
        CHECK(sceneDigest(fixture.scene, textures.resources(), &initialState) == initial);
        CHECK(sceneDigest(fixture.scene, textures.resources(), &changedState) == changed);
        REQUIRE(fixture.scene.restoreLocal(initialState));
        CHECK(sceneDigest(fixture.scene, textures.resources()) == initial);
        CHECK(sceneDigest(fixture.scene, textures.resources(), &changedState) == changed);
        REQUIRE(fixture.scene.restoreLocal(changedState));
        CHECK(sceneDigest(fixture.scene, textures.resources()) == changed);
    }
    // Previous pose changes while the current transform remains identical.
    REQUIRE(fixture.scene.restoreLocal(initialState));
    fixture.scene.setObjectTransform(8, glm::translate(Mat4{1}, Vec3{0, 8, 50}));
    const auto unsettled = sceneDigest(fixture.scene, textures.resources());
    fixture.scene.capturePresentation();
    CHECK(sceneDigest(fixture.scene, textures.resources()) != unsettled);
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.device.textureUpdates == updates);
}

TEST_CASE("world scene digests and draws follow native history across corrections",
          "[world][scene][local-rewind][native-immutable-digest][world-scene-digest]") {
    Fixture fixture("world-digest-static-versions");
    REQUIRE(fixture.build());
    DigestTextures textures(fixture);
    REQUIRE_FALSE(fixture.scene.moving(0));
    REQUIRE_FALSE(fixture.scene.moving(9));
    REQUIRE(fixture.scene.moving(8));
    REQUIRE(fixture.scene.moving(10));
    WorldScene::LocalSnapshot initialState;
    fixture.scene.saveLocal(initialState);
    const auto initial = sceneDigest(fixture.scene, textures.resources());
    REQUIRE(initial);

    fixture.scene.capturePresentation();
    WorldScene::LocalSnapshot settledState;
    fixture.scene.saveLocal(settledState);
    const auto settled = sceneDigest(fixture.scene, textures.resources());
    REQUIRE(settled);
    CHECK(sceneDigest(fixture.scene, textures.resources(), &initialState) == initial);
    const auto step = [&](u32 tick) {
        fixture.scene.capturePresentation();
        fixture.scene.setObjectTransform(8, glm::translate(Mat4{1}, Vec3{0, tick, 50}),
                                         tick % 5 == 0);
        fixture.scene.setObjectAlpha(9, static_cast<f32>(tick % 10) / 10);
        REQUIRE(fixture.scene.setObjectVisible(11, tick % 3 != 0));
        const auto scroll = static_cast<f32>(tick);
        fixture.scene.setTextureScroll(0, {scroll * 0.007f, 0}, {0, scroll * 0.01f}, {0.01f, 0});
        fixture.scene.setTextureTime(tick);
    };
    std::array<WorldScene::LocalSnapshot, 8> reference;
    std::array<NativeStateDigest::Digest, 8> expected{};
    for (usize tick = 0; tick < reference.size(); ++tick) {
        step(static_cast<u32>(tick + 1));
        fixture.scene.saveLocal(reference[tick]);
        const auto digest = sceneDigest(fixture.scene, textures.resources());
        REQUIRE(digest);
        expected[tick] = *digest;
    }
    const auto uploads = fixture.device.texturesCreated;
    const auto updates = fixture.device.textureUpdates;
    for (usize distance = 1; distance <= reference.size(); ++distance) {
        CAPTURE(distance);
        REQUIRE(fixture.scene.restoreLocal(settledState));
        for (usize tick = 0; tick < distance; ++tick) {
            step(static_cast<u32>(100 + tick));
        }
        REQUIRE(fixture.scene.restoreLocal(initialState));
        CHECK(sceneDigest(fixture.scene, textures.resources()) == initial);
        CHECK(sceneDigest(fixture.scene, textures.resources(), &settledState) == settled);
        REQUIRE(fixture.scene.restoreLocal(settledState));
        CHECK(sceneDigest(fixture.scene, textures.resources()) == settled);
        for (usize tick = 0; tick < distance; ++tick) {
            step(static_cast<u32>(tick + 1));
        }
        WorldScene::LocalSnapshot corrected;
        fixture.scene.saveLocal(corrected);
        CHECK(fixture.scene.equalLocal(corrected, reference[distance - 1]));
        CHECK(sceneDigest(fixture.scene, textures.resources()) == expected[distance - 1]);
        for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
            fixture.device.draws.clear();
            fixture.scene.draw(fixture.device, Mat4{1}, CameraFrame{}, alpha);
            const auto draws = fixture.device.draws;
            REQUIRE(fixture.scene.restoreLocal(initialState));
            REQUIRE(fixture.scene.restoreLocal(reference[distance - 1]));
            fixture.device.draws.clear();
            fixture.scene.draw(fixture.device, Mat4{1}, CameraFrame{}, alpha);
            REQUIRE(fixture.device.draws.size() == draws.size());
            for (usize draw = 0; draw < draws.size(); ++draw) {
                test::checkRecordedDraw(fixture.device.draws[draw], draws[draw]);
            }
            CHECK(sceneDigest(fixture.scene, textures.resources()) == expected[distance - 1]);
            CHECK(sceneDigest(fixture.scene, textures.resources(), &initialState) == initial);
        }
    }
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.device.textureUpdates == updates);
    fixture.scene.clear();
    CHECK_FALSE(fixture.scene.canRestoreLocal(initialState));
    CHECK_FALSE(sceneDigest(fixture.scene, {}, &initialState));
}

TEST_CASE("the console's selected object is marked out from its mesh where it stands",
          "[world][scene][dev-console]") {
    Fixture f("world-scene-highlight");
    REQUIRE(f.build());
    const auto& objects = f.layout.objects();
    const auto wall = std::ranges::find_if(
        objects, [](const WorldObject& object) { return object.name == "WALL"; });
    REQUIRE(wall != objects.end());
    const auto index = static_cast<usize>(wall - objects.begin());
    const SelectionHighlight highlight{Vec3{0.0f, 5.0f, -20.0f}};
    f.scene.drawHighlight(f.device, Mat4{1.0f}, index, highlight);
    REQUIRE_FALSE(f.device.draws.empty());
    CHECK(f.device.draws.size() % 2 == 0);
    const Vec3 at{f.scene.worldTransform(index)[3]};
    for (usize i = 0; i < f.device.draws.size(); ++i) {
        const auto& draw = f.device.draws[i];
        CHECK_FALSE(draw.state.depthWrite);
        CHECK(draw.state.depthTest);
        REQUIRE_FALSE(draw.vertices.empty());
        CHECK(draw.vertices[0].color ==
              (i % 2 == 0 ? SelectionHighlight::kRim : SelectionHighlight::kWash));
        // Both lie about where the object stands, not at the origin.
        CHECK(glm::distance(draw.vertices[0].position, at) < 5.0f);
    }
    // A marker with no mesh, or an index past the end, draws nothing.
    const auto marker =
        std::ranges::find_if(objects, [](const WorldObject& object) { return object.particles(); });
    REQUIRE(marker != objects.end());
    f.device.draws.clear();
    f.scene.drawHighlight(f.device, Mat4{1.0f}, static_cast<usize>(marker - objects.begin()),
                          highlight);
    CHECK(f.device.draws.empty());
    f.scene.drawHighlight(f.device, Mat4{1.0f}, objects.size() + 5, highlight);
    CHECK(f.device.draws.empty());
}

} // namespace
