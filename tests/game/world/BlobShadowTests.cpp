#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/BlobShadow.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** An archive holding one flat shadow square's corner. */
std::filesystem::path shadowArchive() {
    const auto root = test::scratchDirectory("blob-shadow");
    writeTextFile(root / "flat.obj", "v 0 0 0\nv 1 0 0\nv 0 0 1\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 1 0\n"
                                     "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
    writeFile(root / "skin.png", test::kTinyPng);
    writeTextFile(root / "objects.json", R"({"objects":[
        {"index":0,"name":"SHADOW2L1","file":"flat.obj","meshTriangles":1}]})");
    writeTextFile(root / "textures.json", R"({"defs":[],"bitmaps":[
        {"index":0,"name":"SHADOW","file":"skin.png","width":2,"height":2,"flags":0}]})");
    writeTextFile(root / "animations.json", R"({"trees":[{"name":"BODY",
        "nodes":[{"name":"BODY","object":"SHADOW2L1","position":[0,0,0]}],"sequences":[]}]})");
    return root;
}

TEST_CASE("a blob shadow binds only the object it names", "[game][world][shadow]") {
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(shadowArchive()));
    BlobShadow shadow;
    REQUIRE_FALSE(shadow.bind(device, archive, "SHADOW1L1"));
    REQUIRE_FALSE(shadow.bound());
    shadow.draw(device, Mat4{1.0f}, Vec3{0, 50, 0}, Vec3{0.0f}, Vec3{0, 1, 0}, {}, 1.0f);
    REQUIRE(device.draws.empty());
    REQUIRE(shadow.bind(device, archive, "SHADOW2L1"));
    REQUIRE(shadow.bound());
    // Seen from straight above, it is drawn that much nearer.
    shadow.draw(device, Mat4{1.0f}, Vec3{4, 50, 6}, Vec3{4, -1, 6}, Vec3{0, 1, 0}, {}, 0.5f);
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws[0];
    REQUIRE_FALSE(draw.state.depthWrite);
    REQUIRE_FALSE(draw.state.cullBack); // the archives' quads face down
    REQUIRE(draw.vertices[0].position.x == Approx(4));
    REQUIRE(draw.vertices[0].position.y == Approx(-1 + BlobShadow::kLift + BlobShadow::kPull));
    REQUIRE(draw.vertices[0].position.z == Approx(6));
    shadow.clear();
    REQUIRE_FALSE(shadow.bound());
}

TEST_CASE("a blob shadow lies along the floor's normal", "[game][world][shadow]") {
    // Level ground keeps the object as authored.
    const Mat4 flat = BlobShadow::placement(Vec3{1, 2, 3}, Vec3{0, 1, 0});
    CHECK(glm::distance(Vec3{flat * Vec4{1, 0, 0, 0}}, Vec3{1, 0, 0}) < 0.0001f);
    CHECK(glm::distance(Vec3{flat * Vec4{0, 0, 1, 0}}, Vec3{0, 0, 1}) < 0.0001f);
    CHECK(glm::distance(Vec3{flat[3]}, Vec3{1, 2 + BlobShadow::kLift, 3}) < 0.0001f);
    // On a slope its up is the slope's and it stays square: lying on the surface.
    const Vec3 slope = glm::normalize(Vec3{1, 1, 0});
    const Mat4 tilted = BlobShadow::placement(Vec3{0}, slope * 3.0f, 2.0f);
    const Vec3 up{tilted[1]};
    CHECK(glm::distance(up, slope * 2.0f) < 0.0001f);
    CHECK(glm::dot(Vec3{tilted[0]}, slope) == Approx(0).margin(0.0001));
    CHECK(glm::dot(Vec3{tilted[2]}, slope) == Approx(0).margin(0.0001));
    CHECK(glm::length(Vec3{tilted[0]}) == Approx(2));
    // Pulled toward the eye, every point keeps to its line of sight, kPull nearer.
    const Vec3 eye{-7, 30, -12};
    const Mat4 pulled = BlobShadow::pulledToward(flat, eye);
    for (const Vec3 point : {Vec3{0}, Vec3{2, 0, -2}, Vec3{-2, 0, 2}}) {
        const Vec3 before{flat * Vec4{point, 1}};
        const Vec3 after{pulled * Vec4{point, 1}};
        CHECK(glm::length(glm::cross(glm::normalize(before - eye), glm::normalize(after - eye))) <
              0.0001f);
        CHECK(glm::distance(after, eye) < glm::distance(before, eye));
    }
    CHECK(glm::distance(Vec3{pulled[3]}, eye) ==
          Approx(glm::distance(Vec3{flat[3]}, eye) - BlobShadow::kPull));
    // An eye nearer than that leaves it be.
    CHECK(BlobShadow::pulledToward(flat, Vec3{flat[3]} + Vec3{0, 0.1f, 0}) == flat);
    // A normal of nothing, or along the x axis, falls back to lying flat.
    for (const Vec3 bad : {Vec3{0}, Vec3{1, 0, 0}}) {
        const Mat4 fallback = BlobShadow::placement(Vec3{0}, bad);
        CHECK(glm::distance(Vec3{fallback[1]}, Vec3{0, 1, 0}) < 0.0001f);
    }
}
} // namespace
