#include <array>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/ui/SystemFont.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/BuildLabel.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("build labels present the complete SemVer release", "[game][build-label]") {
    CHECK(BuildLabel::formatVersion("0.1.0-alpha.2") == "Build v0.1.0-alpha.2");
    CHECK(BuildLabel::formatVersion("1.2.3-beta.10") == "Build v1.2.3-beta.10");
    CHECK(BuildLabel::formatVersion("1.2.3-rc.1") == "Build v1.2.3-rc.1");
    CHECK(BuildLabel::formatVersion("1.2.3-preview") == "Build v1.2.3-preview");
    CHECK(BuildLabel::formatVersion("1.2.3") == "Build v1.2.3");
    CHECK(BuildLabel::formatVersion("1.2.3+build.7") == "Build v1.2.3+build.7");
    CHECK(BuildLabel::formatVersion("1.2.3-beta.2+sha-with-dashes") ==
          "Build v1.2.3-beta.2+sha-with-dashes");
}

TEST_CASE("the build label stays at the viewport corner across aspect ratios",
          "[game][build-label]") {
    for (const Extent2D extent :
         {Extent2D{640, 480}, Extent2D{1920, 1080}, Extent2D{3440, 1440}, Extent2D{600, 900}}) {
        const auto matrix = BuildLabel::projection(extent);
        const Vec4 origin = matrix * Vec4{0, 0, 0, 1};
        CHECK(origin.x == Approx(-1));
        CHECK(origin.y == Approx(-1));
        const Vec4 margin = matrix * Vec4{6, 6, 0, 1};
        const f32 x = (margin.x + 1) * static_cast<f32>(extent.width) / 2;
        const f32 y = (margin.y + 1) * static_cast<f32>(extent.height) / 2;
        CHECK(x == Approx(y).margin(0.001));
    }
}

TEST_CASE("the build overlay uses system text, white three-quarter alpha and no depth occlusion",
          "[game][build-label]") {
    test::FakeRenderDevice device;
    BuildLabel label("0.1.0-alpha.2");
    CHECK(label.text() == "Build v0.1.0-alpha.2");
    label.render(device, {1920, 1080});
    CHECK(device.draws.empty());
    REQUIRE(label.load(device));
    REQUIRE(label.ready());
    const u32 uploads = device.texturesCreated;
    REQUIRE(uploads == 1);
    label.render(device, {1920, 1080});
    REQUIRE(device.draws.size() == 1);
    for (const auto& draw : device.draws) {
        CHECK_FALSE(draw.state.depthTest);
        CHECK_FALSE(draw.state.depthWrite);
        CHECK(draw.blend() == BlendMode::Alpha);
        for (const auto& vertex : draw.vertices) {
            CHECK(vertex.color == Color::rgba(255, 255, 255, 191));
        }
        CHECK(draw.vertices.size() == 6);
        CHECK(test::minCorner(draw).x == Approx(6));
        CHECK(test::minCorner(draw).y == Approx(6));
        CHECK(test::maxCorner(draw).y - test::minCorner(draw).y == Approx(12));
        const auto* sheet = dynamic_cast<const test::FakeTexture*>(draw.texture);
        REQUIRE(sheet != nullptr);
        CHECK(sheet->pixels == rasterizeSystemText(label.text(), 48).pixels);
    }
    label.render(device, {640, 480});
    CHECK(device.texturesCreated == uploads);
    CHECK(device.textureUpdates == 0);
    device.draws.clear();
    label.render(device, {0, 0});
    CHECK(device.draws.empty());
    label.release();
    CHECK_FALSE(label.ready());
    label.render(device, {640, 480});
    CHECK(device.draws.empty());
}

TEST_CASE("system text has clean antialiased coverage without retail assets", "[ui][build-label]") {
    const auto image = rasterizeSystemText("Build v0.1.0-alpha.2", 48);
    REQUIRE(image.width > 100);
    REQUIRE(image.height >= 48);
    std::array<usize, 3> coverage{};
    bool white = true;
    for (usize i = 0; i < image.pixels.size(); i += 4) {
        white = white && image.pixels[i] == 255 && image.pixels[i + 1] == 255 &&
                image.pixels[i + 2] == 255;
        const auto alpha = image.pixels[i + 3];
        if (alpha == 0) {
            ++coverage[0];
        } else if (alpha == 255) {
            ++coverage[2];
        } else {
            ++coverage[1];
        }
    }
    CHECK(white);
    CHECK(coverage[0] > 0);
    CHECK(coverage[1] > 0);
    CHECK(coverage[2] > 0);
    CHECK(rasterizeSystemText("", 48).pixels.empty());
    CHECK(rasterizeSystemText("Build v0.1.0-alpha.2", 24).width < image.width);
    CHECK_THROWS_AS(rasterizeSystemText("test", 0), FormatError);
    CHECK_THROWS_AS(rasterizeSystemText("test", 129), FormatError);
    CHECK_THROWS_AS(rasterizeSystemText("two\nlines", 48), FormatError);
    CHECK_THROWS_AS(rasterizeSystemText(std::string(257, 'A'), 48), FormatError);
}
} // namespace
