#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/math/Math.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"

namespace {

using namespace gdl;
using Catch::Approx;

TEST_CASE("consecutive draws with one texture share a batch", "[ui][canvas]") {
    test::FakeRenderDevice device;
    const test::FakeTexture a{4, 4};
    const test::FakeTexture b{4, 4};
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    canvas.draw(a, Rect{0.0f, 0.0f, 10.0f, 10.0f});
    canvas.draw(a, Rect{10.0f, 0.0f, 10.0f, 10.0f});
    canvas.draw(b, Rect{20.0f, 0.0f, 10.0f, 10.0f});
    canvas.draw(a, Rect{30.0f, 0.0f, 10.0f, 10.0f});
    REQUIRE(device.draws.size() == 2);
    canvas.end();
    REQUIRE(device.draws.size() == 3);
    REQUIRE(device.draws[0].texture == &a);
    REQUIRE(device.draws[0].vertices.size() == 12);
    REQUIRE(device.draws[1].texture == &b);
    REQUIRE(device.draws[1].vertices.size() == 6);
    REQUIRE(device.draws[2].texture == &a);
    REQUIRE(test::minCorner(device.draws[2]).x == Approx(30.0f));
    REQUIRE(test::maxCorner(device.draws[2]).x == Approx(40.0f));
    REQUIRE_FALSE(canvas.active());
}

TEST_CASE("fills use the device's white texture and uvs are passed through", "[ui][canvas]") {
    test::FakeRenderDevice device;
    const test::FakeTexture a{4, 4};
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    canvas.fill(Rect{0.0f, 0.0f, 2.0f, 2.0f}, Color::black());
    canvas.draw(a, Rect{0.0f, 0.0f, 2.0f, 2.0f}, Rect{0.25f, 0.5f, 0.5f, 0.25f}, Color::white());
    canvas.end();
    REQUIRE(device.draws.size() == 2);
    REQUIRE(device.draws[0].texture == &device.whiteTexture());
    REQUIRE(device.draws[0].vertices[0].color == Color::black());
    bool sawUv = false;
    for (const ImmediateVertex& v : device.draws[1].vertices) {
        sawUv = sawUv || (v.uv.x == Approx(0.75f) && v.uv.y == Approx(0.75f));
    }
    REQUIRE(sawUv);
}

TEST_CASE("nothing is submitted for an empty canvas", "[ui][canvas]") {
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    canvas.end();
    REQUIRE(device.draws.empty());
}

TEST_CASE("filled arrows preserve canvas draw order and ignore scene depth", "[ui][canvas]") {
    test::FakeRenderDevice device;
    Canvas canvas;
    const Mat4 projection = makeScreenProjection(512, 384);
    canvas.begin(device, projection);
    canvas.fill({0, 0, 1, 1}, Color::black());
    canvas.fillTriangle({5, 10}, {10, 5}, {10, 15}, Color::white());
    canvas.fill({20, 0, 1, 1}, Color::black());
    canvas.end();
    REQUIRE(device.draws.size() == 3);
    const auto& arrow = device.draws[1];
    REQUIRE(arrow.vertices.size() == 3);
    CHECK(Vec2(arrow.vertices[0].position) == Vec2{5, 10});
    CHECK(Vec2(arrow.vertices[1].position) == Vec2{10, 5});
    CHECK(Vec2(arrow.vertices[2].position) == Vec2{10, 15});
    CHECK(arrow.texture == &device.whiteTexture());
    CHECK(arrow.transform == projection);
    CHECK_FALSE(arrow.state.depthTest);
    CHECK_FALSE(arrow.state.depthWrite);
}

TEST_CASE("screen fills cover widescreen margins without changing the canvas transform",
          "[ui][canvas]") {
    test::FakeRenderDevice device;
    Canvas canvas;
    const auto transform = makeLetterboxProjection(512, 384, 1920, 1080);
    const Color dim = Color::rgba(0, 0, 0, 150);
    canvas.begin(device, transform);
    canvas.fill({0, 0, 512, 384}, Color::white());
    canvas.fillScreen(dim);
    canvas.fill({0, 0, 1, 1}, Color::white());
    canvas.end();
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws[0].transform == transform);
    CHECK(device.draws[1].transform == Mat4{1});
    CHECK(test::minCorner(device.draws[1]) == Vec2{-1, -1});
    CHECK(test::maxCorner(device.draws[1]) == Vec2{1, 1});
    CHECK_FALSE(device.draws[1].state.depthTest);
    CHECK_FALSE(device.draws[1].state.depthWrite);
    for (const auto& vertex : device.draws[1].vertices) {
        CHECK(vertex.color == dim);
    }
    CHECK(device.draws[2].transform == transform);
}

TEST_CASE("canvas masks only unused margins at standard wide ultrawide and portrait ratios",
          "[ui][canvas]") {
    const auto extent = GENERATE(Extent2D{640, 448}, Extent2D{1280, 720}, Extent2D{3440, 1440},
                                 Extent2D{600, 1000});
    test::FakeRenderDevice device;
    Canvas canvas;
    const auto transform =
        makeVirtualScreenTransform(makeLetterboxProjection(640, 448, static_cast<f32>(extent.width),
                                                           static_cast<f32>(extent.height)),
                                   512, 384, 640, 448);
    canvas.begin(device, transform);
    canvas.maskOutside({0, 0, 512, 384});
    canvas.end();
    if (extent.width == 640) {
        CHECK(device.draws.empty());
        return;
    }
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws.front();
    CHECK(draw.transform == Mat4{1});
    CHECK_FALSE(draw.state.depthTest);
    CHECK_FALSE(draw.state.depthWrite);
    const Vec4 low = transform * Vec4{0, 0, 0, 1};
    const Vec4 high = transform * Vec4{512, 384, 0, 1};
    f32 area = 0;
    for (usize i = 0; i < draw.vertices.size(); i += 3) {
        const Vec2 a{draw.vertices[i].position};
        const Vec2 b{draw.vertices[i + 1].position};
        const Vec2 c{draw.vertices[i + 2].position};
        const Vec2 middle = (a + b + c) / 3.0f;
        CHECK((middle.x <= low.x || middle.x >= high.x || middle.y <= low.y || middle.y >= high.y));
        area += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
        CHECK(draw.vertices[i].color == Color::black());
    }
    CHECK(area == Approx(4 - (high.x - low.x) * (high.y - low.y)));
}

TEST_CASE("cutscene bands extend across widescreen without masking the side view",
          "[ui][canvas][cutscene]") {
    const auto extent = GENERATE(Extent2D{640, 480}, Extent2D{1920, 1080}, Extent2D{3440, 1440});
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, makeLetterboxProjection(512, 384, static_cast<f32>(extent.width),
                                                 static_cast<f32>(extent.height)));
    canvas.fillHorizontalBand(0, 48, Color::black());
    canvas.fillHorizontalBand(304, 80, Color::black());
    canvas.end();
    REQUIRE(device.draws.size() == 2);
    CHECK(test::minCorner(device.draws[0]) == Vec2{-1, -1});
    CHECK(test::maxCorner(device.draws[0]) == Vec2{1, -0.75f});
    CHECK(test::minCorner(device.draws[1]).x == -1);
    CHECK(test::minCorner(device.draws[1]).y == Approx(304.0f / 192.0f - 1.0f));
    CHECK(test::maxCorner(device.draws[1]).x == Approx(1));
    CHECK(test::maxCorner(device.draws[1]).y == Approx(1));
    for (const auto& draw : device.draws) {
        CHECK_FALSE(draw.state.depthTest);
        CHECK_FALSE(draw.state.depthWrite);
    }
}

TEST_CASE("the virtual screen transform stretches onto the frame", "[ui][canvas]") {
    const Mat4 projection = makeScreenProjection(640.0f, 448.0f);
    const Mat4 transform = makeVirtualScreenTransform(projection, 512.0f, 384.0f, 640.0f, 448.0f);
    const Vec4 corner = transform * Vec4{512.0f, 384.0f, 0.0f, 1.0f};
    const Vec4 expected = projection * Vec4{640.0f, 448.0f, 0.0f, 1.0f};
    REQUIRE(corner.x == Approx(expected.x));
    REQUIRE(corner.y == Approx(expected.y));
    const Vec4 middle = transform * Vec4{256.0f, 192.0f, 0.0f, 1.0f};
    REQUIRE(middle.x == Approx(0.0f).margin(1e-5));
    REQUIRE(middle.y == Approx(0.0f).margin(1e-5));
}

} // namespace
