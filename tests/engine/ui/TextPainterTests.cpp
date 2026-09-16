#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"

namespace {

using namespace gdl;
using Catch::Approx;

BitmapFont sampleFont() {
    return BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
}

TEST_CASE("text is measured from glyph widths and the space width", "[ui][text]") {
    const BitmapFont font = sampleFont();
    const test::FakeTexture sheet{32, 16};
    TextPainter painter;
    REQUIRE_FALSE(painter.ready());
    painter.setFont(&font, &sheet);
    REQUIRE(painter.ready());
    REQUIRE(painter.measure("AB") == 14);
    REQUIRE(painter.measure("A B") == 18);
    REQUIRE(painter.measure("A?B") == 14);
    REQUIRE(painter.measure("AB", 2.0f) == 28);
    REQUIRE(painter.lineHeight() == 10);
    REQUIRE(painter.lineHeight(0.5f) == 5);
    REQUIRE(painter.leftEdge(-100, "AB", 1.0f) == 93);
    REQUIRE(painter.leftEdge(40, "AB", 1.0f) == 40);
}

TEST_CASE("glyphs are drawn as cells of the sheet in one batch", "[ui][text]") {
    const BitmapFont font = sampleFont();
    const test::FakeTexture sheet{32, 16};
    test::FakeRenderDevice device;
    TextPainter painter;
    painter.setFont(&font, &sheet);
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    const s32 end = painter.draw(canvas, 10, 20, "A B", TextStyle{});
    canvas.end();
    REQUIRE(end == 28);
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws[0].texture == &sheet);
    REQUIRE(device.draws[0].vertices.size() == 12);
    const Vec2 first = test::minCorner(device.draws[0]);
    REQUIRE(first.x == Approx(10.0f));
    REQUIRE(first.y == Approx(20.0f));
    const Vec2 last = test::maxCorner(device.draws[0]);
    REQUIRE(last.x == Approx(28.0f));
    REQUIRE(last.y == Approx(30.0f));
    bool sawCellB = false;
    for (const ImmediateVertex& v : device.draws[0].vertices) {
        sawCellB = sawCellB || (v.uv.x == Approx(14.0f / 32.0f) && v.uv.y == Approx(10.0f / 16.0f));
    }
    REQUIRE(sawCellB);
}

TEST_CASE("styles scale, recolour, expand and swap the sheet", "[ui][text]") {
    const BitmapFont font = sampleFont();
    const test::FakeTexture sheet{32, 16};
    const test::FakeTexture glow{32, 16};
    test::FakeRenderDevice device;
    TextPainter painter;
    painter.setFont(&font, &sheet);
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    TextStyle style;
    style.scale = 2.0f;
    style.color = Color::rgba(1, 2, 3, 4);
    style.texture = &glow;
    style.expand = 2;
    painter.draw(canvas, -50, 0, "A", style);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws[0].texture == &glow);
    REQUIRE(device.draws[0].vertices[0].color == Color::rgba(1, 2, 3, 4));
    REQUIRE(test::minCorner(device.draws[0]).x == Approx(42.0f));
    REQUIRE(test::minCorner(device.draws[0]).y == Approx(-2.0f));
    REQUIRE(test::maxCorner(device.draws[0]).x == Approx(58.0f));
    REQUIRE(test::maxCorner(device.draws[0]).y == Approx(22.0f));
}

} // namespace
