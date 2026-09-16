#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
