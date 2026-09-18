#include <array>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"
#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    TextureSet lender;
    WorldLayout layout;
    WorldScene scene;

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
        REQUIRE(lender.load(test::sampleLender(std::string(name) + "-lender")));
    }

    bool build() {
        const std::array<TextureSet*, 1> lenders{&lender};
        return scene.build(layout, models, textures, device, WorldLighting{}, lenders);
    }
};

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
    // The opaque wall draws first, offset by its parent, then the lightmapped floor (its own
    // batch under the same texture) and the moving blade; the translucent window follows;
    // the sorted units come farthest first; the glowing copy of the wall adds itself last.
    const auto& wall = f.device.draws[0];
    REQUIRE(wall.vertices.size() == 3);
    REQUIRE(wall.vertices[0].position == Vec3{11.0f, 0.0f, 0.0f});
    REQUIRE(wall.vertices[1].position == Vec3{12.0f, 0.0f, 0.0f});
    REQUIRE(wall.blend() == BlendMode::Alpha);
    REQUIRE(wall.lightmap() == nullptr);
    REQUIRE(wall.state.cullBack);
    REQUIRE(wall.state.depthWrite);
    REQUIRE(wall.state.alphaTest == 0.0f);
    const auto& floor = f.device.draws[1];
    REQUIRE(floor.texture == wall.texture);
    REQUIRE(floor.lightmap() == &f.textures.texture(f.device, 2));
    REQUIRE(floor.vertices[0].position == Vec3{10.0f, 0.0f, 20.0f}); // under the group
    REQUIRE(floor.vertices[1].uv2 == Vec2{0.5f, 0.25f}); // texels over the 2x2 lightmap
    const auto& blade = f.device.draws[2];
    REQUIRE(blade.texture == wall.texture);
    REQUIRE(blade.vertices[0].position == Vec3{11.0f, 0.0f, 50.0f});
    const auto& window = f.device.draws[3];
    REQUIRE(window.vertices[0].position == Vec3{10.0f, 5.0f, 0.0f});
    REQUIRE(window.state.alphaTest == WorldScene::kAlphaTest);
    REQUIRE(window.state.depthWrite);
    // Lit from above, the upward-facing wall is brighter than the downward-facing window,
    // which keeps the ambient alone.
    REQUIRE(wall.vertices[0].color.r > window.vertices[0].color.r);
    REQUIRE(window.vertices[0].color.r == 140);
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
    f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 200.0f});
    REQUIRE(f.device.draws[4].vertices[0].position == Vec3{10.0f, 0.0f, 30.0f});
    REQUIRE(f.device.draws[6].vertices[0].position == Vec3{10.0f, 0.0f, 100.0f});

    f.scene.clear();
    REQUIRE_FALSE(f.scene.built());
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1.0f});
    REQUIRE(f.device.draws.empty());
}

TEST_CASE("moving objects carry what stands under them", "[world][scene]") {
    Fixture f("world-scene-moving");
    REQUIRE(f.build());
    // A quarter turn of the spinning group swings its blade from +x to -z.
    const Mat4 turned = glm::rotate(glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.0f, 50.0f}),
                                    kHalfPi, Vec3{0.0f, 1.0f, 0.0f});
    f.scene.setObjectTransform(8, turned);
    f.scene.setObjectTransform(2, turned); // the still wall ignores it
    f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 0.0f});
    const auto& blade = f.device.draws[2];
    REQUIRE(blade.vertices[0].position.x == Approx(10.0f).margin(1e-5f));
    REQUIRE(blade.vertices[0].position.z == Approx(49.0f).margin(1e-5f));
    REQUIRE(blade.vertices[1].position.z == Approx(48.0f).margin(1e-5f));
    REQUIRE(f.device.draws[0].vertices[0].position == Vec3{11.0f, 0.0f, 0.0f});
}

TEST_CASE("texture slots can show another frame or slide their coordinates",
          "[world][scene]") {
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
    REQUIRE(f.device.draws[3].state.uvOffset == Vec2{0.25f, 0.0f}); // the window
    REQUIRE(f.device.draws[0].state.uvOffset == Vec2{0.0f, 0.0f});
    f.scene.setTextureFrame(0, nullptr);
    REQUIRE(f.scene.textureOf(0) == &f.textures.texture(f.device, 0));
    // Slots the scene never draws are ignored.
    f.scene.setTextureFrame(99, &frame);
    f.scene.setTextureOffset(99, Vec2{1.0f, 1.0f});
    REQUIRE(f.scene.textureOffset(99) == Vec2{0.0f, 0.0f});
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

TEST_CASE("a prelit object is shaded by its vertices, not the lights", "[world][scene]") {
    Fixture f("world-scene-prelit");
    // Two copies of the same dim wall: one flagged prelit, one left to the lights.
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
    bool dim = false;
    bool lit = false;
    for (const ImmediateVertex& v : vertices) {
        dim = dim || v.color.r == 51;  // 0.2 of the way, its own colour
        lit = lit || v.color.r > 200;  // the lit copy, facing the light
    }
    REQUIRE(dim);
    REQUIRE(lit);
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
    writeTextFile(dir / "world.json",
                  R"({"objects": [{"name": "GHOST", "position": [0, 0, 0]}]})");
    WorldLayout ghosts;
    REQUIRE(ghosts.load(dir));
    REQUIRE_FALSE(f.scene.build(ghosts, f.models, f.textures, f.device));
    REQUIRE(f.scene.placedCount() == 0);
}

} // namespace
