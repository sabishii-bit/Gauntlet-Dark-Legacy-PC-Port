#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/menu/CompassHud.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kFov = kPi / 3.0f;
constexpr f32 kAspect = 640.0f / 448.0f;

TEST_CASE("compass keeps its retail screen anchor and world axes through camera motion",
          "[compass]") {
    // StartCompass and its updater use COMPASS, (64,128), depth 10, scale 1.5;
    // MBWindowTo3D translates the object but does not turn its identity basis.
    for (const f32 yaw : {0.0f, kPi / 2.0f, kPi}) {
        WorldCamera camera;
        camera.position = Vec3{12, 30, -20};
        camera.yaw = yaw;
        camera.pitch = 0.7f;
        camera.roll = 0.2f;
        const Mat4 placement = CompassHud::placement(camera, kFov, kAspect);
        CHECK(Vec3{placement[0]} == Vec3{1.5f, 0, 0});
        CHECK(Vec3{placement[1]} == Vec3{0, 1.5f, 0});
        CHECK(Vec3{placement[2]} == Vec3{0, 0, 1.5f});
        const Vec4 eye = camera.view() * placement[3];
        CHECK(eye.z == Approx(10).margin(0.0001f));
        const Vec4 projected =
            WorldCamera::frameMapping(640, 448) * WorldCamera::projection(kFov, kAspect) * eye;
        CHECK(projected.x / projected.w == Approx(64).margin(0.001f));
        CHECK(projected.y / projected.w == Approx(128).margin(0.001f));

        const Mat4 letterbox = makeLetterboxProjection(640, 448, 2560, 896);
        const Vec4 clip = camera.clipTransform(kFov, 640, 448, letterbox) * placement[3];
        const Vec4 expected = letterbox * Vec4{64, 128, 0, 1};
        CHECK(clip.x / clip.w == Approx(expected.x).margin(0.0001f));
        CHECK(clip.y / clip.w == Approx(expected.y).margin(0.0001f));
    }
}

TEST_CASE("compass draws a native model with retail transparency and clears borrowed resources",
          "[compass]") {
    const auto directory = test::scratchDirectory("compass-native");
    writeTextFile(directory / "compass.obj", "v 0 0 0\nv 1 0 0\nv 0 0 1\nvn 0 1 0\n"
                                             "usemtl tex0\nf 1//1 2//1 3//1\n");
    writeFile(directory / "compass.png", test::kTinyPng);
    writeTextFile(directory / "objects.json", R"({"objects":[
        {"index":0,"name":"COMPASS","file":"compass.obj","meshTriangles":1}]})");
    writeTextFile(directory / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"COMPASS","file":"compass.png","width":2,"height":2}]})");
    test::convertModelFixture(directory);
    ItemArchive archive;
    REQUIRE(archive.models.load(directory));
    REQUIRE(archive.textures.load(directory));
    test::FakeRenderDevice device;
    CompassHud compass;
    REQUIRE(compass.bind(device, archive));
    const WorldCamera camera;
    compass.draw(device, Mat4{1}, camera, kFov, kAspect, {}, false);
    CHECK(device.draws.empty());
    compass.draw(device, Mat4{1}, camera, kFov, kAspect, {});
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws.front();
    CHECK(draw.vertices.size() == 3);
    CHECK(draw.state.blend == BlendMode::Alpha);
    CHECK_FALSE(draw.state.depthWrite);
    for (const auto& vertex : draw.vertices) {
        CHECK(vertex.color.a == 127);
    }
    compass.clear();
    archive.clear();
    device.draws.clear();
    compass.draw(device, Mat4{1}, camera, kFov, kAspect, {});
    CHECK(device.draws.empty());
    CHECK_FALSE(compass.bind(device, archive));
    CHECK_FALSE(compass.bound());
}

TEST_CASE("retail compass is the standalone POWERUPS mesh rather than cardinal text",
          "[compass][assets]") {
    const auto directory = test::assetOrSkip("POWERUPS/objects.ngc").parent_path();
    ItemArchive archive;
    REQUIRE(archive.models.load(directory));
    REQUIRE(archive.textures.load(directory));
    const auto index = archive.models.find("COMPASS");
    REQUIRE(index.has_value());
    const auto& mesh = archive.models.mesh(*index);
    CHECK(mesh.triangleCount() == 32);
    test::FakeRenderDevice device;
    CompassHud compass;
    REQUIRE(compass.bind(device, archive));
    WorldCamera camera;
    camera.pitch = 0.7f;
    compass.draw(device, Mat4{1}, camera, kFov, kAspect, {});
    usize vertices = 0;
    for (const auto& draw : device.draws) {
        vertices += draw.vertices.size();
        CHECK(draw.texture != &device.whiteTexture());
    }
    CHECK(vertices == 96);
    compass.clear();
    archive.clear();
}
} // namespace
