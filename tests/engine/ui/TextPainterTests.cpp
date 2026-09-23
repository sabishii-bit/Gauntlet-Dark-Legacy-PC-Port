#include <cstdint>

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
    const std::int32_t end = painter.draw(canvas, 10, 20, "A B", TextStyle{});
    canvas.end();
    REQUIRE(end == 28);
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws[0].texture == &sheet);
    REQUIRE(device.draws[0].vertices.size() == 12);
    // Each cell is drawn half a texel inside its borders, so the filtering never pulls in
    // the sheet's grid or the next glyph.
    const Vec2 first = test::minCorner(device.draws[0]);
    REQUIRE(first.x == Approx(10.5f));
    REQUIRE(first.y == Approx(20.5f));
    const Vec2 last = test::maxCorner(device.draws[0]);
    REQUIRE(last.x == Approx(27.5f));
    REQUIRE(last.y == Approx(29.5f));
    bool sawCellB = false;
    for (const ImmediateVertex& v : device.draws[0].vertices) {
        sawCellB = sawCellB || (v.uv.x == Approx(13.5f / 32.0f) && v.uv.y == Approx(9.5f / 16.0f));
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
    // Scaled twice and expanded two: the cell's half-texel inset scales with it.
    REQUIRE(test::minCorner(device.draws[0]).x == Approx(43.0f));
    REQUIRE(test::minCorner(device.draws[0]).y == Approx(-1.0f));
    REQUIRE(test::maxCorner(device.draws[0]).x == Approx(57.0f));
    REQUIRE(test::maxCorner(device.draws[0]).y == Approx(21.0f));
}

} // namespace
