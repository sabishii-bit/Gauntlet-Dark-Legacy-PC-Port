#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/scenes/LegendsWorldPresentation.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("N64 world flags keep sorting blending depth and facing independent",
          "[game][legends][world-presentation]") {
    const auto flame = LegendsWorldPresentation::n64(0x09001800);
    REQUIRE(flame.facing == 1);
    REQUIRE(flame.sortable);
    REQUIRE(flame.unlit);
    REQUIRE(flame.depthWrite);
    REQUIRE(flame.depthTest);
    REQUIRE_FALSE(flame.forceAlpha);
    REQUIRE_FALSE(flame.hidden);
    const auto snow = LegendsWorldPresentation::n64(0x08001800);
    REQUIRE(snow.facing == 0);
    REQUIRE(snow.sortable);
    REQUIRE(snow.unlit);
    const auto lava = LegendsWorldPresentation::n64(0x08001000);
    REQUIRE(lava.facing == 0);
    REQUIRE_FALSE(lava.sortable);
    REQUIRE(lava.unlit);
    const auto hidden = LegendsWorldPresentation::n64(2);
    REQUIRE(hidden.hidden);
    REQUIRE(hidden.depthWrite);
    const auto alpha = LegendsWorldPresentation::n64(0x10000080);
    REQUIRE(alpha.forceAlpha);
    REQUIRE_FALSE(alpha.depthWrite);
    REQUIRE(alpha.depthTest);
    REQUIRE_FALSE(alpha.sortable);
    const auto noCompare = LegendsWorldPresentation::n64(0x40);
    REQUIRE(noCompare.depthWrite);
    REQUIRE_FALSE(noCompare.depthTest);
    // N64's 0x800000 is a texture-scale choice, not GameCube's additive flag.
    REQUIRE_FALSE(LegendsWorldPresentation::n64(0x800000).forceAlpha);
}

TEST_CASE("N64 upright world planes rotate toward the view without moving their anchors",
          "[game][legends][world-presentation]") {
    const auto flame = LegendsWorldPresentation::n64(0x01000000);
    const Mat4 authored = glm::translate(Mat4{1}, Vec3{3, 4, 5});
    WorldCamera camera;
    camera.position = {3, 4, -5};
    auto placed = flame.placement(authored, camera);
    REQUIRE(placed[0].x == Catch::Approx(-1));
    REQUIRE(placed[2].z == Catch::Approx(-1));
    REQUIRE(placed[3] == authored[3]);
    REQUIRE(placed[1] == authored[1]);
    camera.position = {13, 4, 5};
    camera.yaw = -kPi / 2;
    placed = flame.placement(authored, camera);
    REQUIRE(placed[2].x == Catch::Approx(1));
    REQUIRE(placed[0].z == Catch::Approx(-1));
    REQUIRE(placed[3] == authored[3]);
    const auto turned = glm::rotate(authored, 0.4f, Vec3{0, 1, 0});
    const auto scaled = glm::scale(turned, Vec3{2, 3, 2});
    const auto faced = flame.placement(scaled, camera);
    REQUIRE(faced[2].x == Catch::Approx(2));
    REQUIRE(glm::length(Vec3{faced[0]}) == Catch::Approx(2));
    REQUIRE(glm::length(Vec3{faced[1]}) == Catch::Approx(3));
    REQUIRE(glm::length(Vec3{faced[2]}) == Catch::Approx(2));
    REQUIRE(faced[3] == authored[3]);
}

TEST_CASE("N64 upright facing follows native projected depth rather than world-XZ look-at",
          "[game][legends][world-presentation]") {
    const auto flame = LegendsWorldPresentation::n64(0x01000000);
    const Mat4 authored = glm::translate(Mat4{1}, Vec3{5, 0, 0});
    WorldCamera camera;
    camera.position = {0, 10, -10};
    camera.pitch = kPi / 4;
    const auto faced = flame.placement(authored, camera);
    // Native view centre is (5,0,10*sqrt(2)): yaw sine=-1/3,
    // cosine=-sqrt(8)/3. A world-XZ look-at would incorrectly use -1/sqrt(5).
    REQUIRE(faced[2].x == Catch::Approx(-1.0f / 3));
    REQUIRE(faced[2].z == Catch::Approx(-std::sqrt(8.0f) / 3));
    REQUIRE(faced[1] == authored[1]);
    REQUIRE(faced[3] == authored[3]);
}

TEST_CASE("N64 world presentation does not reinterpret unknown facing modes or invalid views",
          "[game][legends][world-presentation]") {
    const Mat4 authored = glm::translate(Mat4{1}, Vec3{7, 8, 9});
    WorldCamera camera;
    REQUIRE(LegendsWorldPresentation::n64(0).placement(authored, camera) == authored);
    REQUIRE(LegendsWorldPresentation::n64(0x02000000).placement(authored, camera) == authored);
    camera.pitch = std::numeric_limits<f32>::quiet_NaN();
    REQUIRE(LegendsWorldPresentation::n64(0x01000000).placement(authored, camera) == authored);
}

TEST_CASE("N64 dynamic decal depth follows the native camera-vector quantization",
          "[game][legends][world-presentation][native-decals]") {
    const auto cracks = LegendsWorldPresentation::n64(0x08000800, 0x18000001);
    const auto projectedLight = LegendsWorldPresentation::n64(0x08001800, 0x18000001);
    REQUIRE(cracks.cameraDepthPull);
    REQUIRE(projectedLight.cameraDepthPull);
    REQUIRE(cracks.depthWrite);
    REQUIRE(cracks.depthTest);
    // Native static-matrix decals are a different rendering path, and a light
    // beam's no-collision flag is not a request for the dynamic depth adjustment.
    REQUIRE_FALSE(LegendsWorldPresentation::n64(0x08000000, 0x18000001).cameraDepthPull);
    REQUIRE_FALSE(LegendsWorldPresentation::n64(0x08001800, 0x48000001).cameraDepthPull);
    REQUIRE(LegendsWorldPresentation::n64(0x10000).cameraDepthPull);
    WorldCamera camera;
    CHECK(Vec3{cracks.drawAdjustment(camera)[3]} == Vec3(0, 0, -1.5f));
    camera.pitch = .3f;
    camera.yaw = -.7f;
    camera.roll = .6f;
    const Vec3 backward = -camera.forward();
    const auto adjustment = cracks.drawAdjustment(camera);
    for (usize axis = 0; axis < 3; ++axis) {
        CHECK(adjustment[3][axis] ==
              Catch::Approx(std::trunc(backward[axis] * 4096.0f) * 3.0f / 8192.0f));
    }
    CHECK(glm::length(Vec3{adjustment[3]}) <= 1.5f);
    const auto before = adjustment;
    camera.position = {4000, -3000, 2500};
    CHECK(cracks.drawAdjustment(camera) == before);
    camera.pitch = std::numeric_limits<f32>::quiet_NaN();
    CHECK(cracks.drawAdjustment(camera) == Mat4{1});
}
