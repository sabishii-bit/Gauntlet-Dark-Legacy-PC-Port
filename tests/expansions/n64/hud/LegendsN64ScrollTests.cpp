#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "RecordedQuads.h"
#include "expansions/n64/hud/LegendsN64Frame.h"
#include "expansions/n64/hud/LegendsN64Scroll.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using test::Quad;
using test::quadsOf;

/** Checks `actual` lies on `expected`: the scroll's rects are whole frame units and halves of
 * texels, within a hundred-thousandth. */
void checkRect(const Rect& actual, const Rect& expected) {
    constexpr f64 kRounding = 1e-5;
    test::checkRect(actual, expected, kRounding);
}

/** The quads of `scroll` drawn over `area` in the N64's frame. */
std::vector<Quad> scrollOver(const Texture& scroll, const Rect& area) {
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    {
        const CanvasFrame frame(canvas, LegendsN64Frame::transform());
        drawLegendsN64Scroll(canvas, scroll, area);
    }
    canvas.end();
    return quadsOf(device);
}

} // namespace

TEST_CASE("the N64 scroll keeps its caps and stretches its middle rows", "[legends][n64-scroll]") {
    // TEXTSCROLL and the coloured scrolls are 64 by 32.
    const test::FakeTexture scroll(64, 32);
    // Each piece is sampled half a texel inside its rows and the picture's sides.
    const Rect topRows{0.5f / 64, 0.5f / 32, 63.0f / 64, 7.0f / 32};
    const Rect middleRows{0.5f / 64, 8.5f / 32, 63.0f / 64, 14.0f / 32};
    const Rect bottomRows{0.5f / 64, 23.5f / 32, 63.0f / 64, 7.0f / 32};
    for (const f32 height : {27.0f, 60.0f, 200.0f}) {
        CAPTURE(height);
        const std::vector<Quad> quads = scrollOver(scroll, {40, 50, 350, height});
        REQUIRE(quads.size() == 3);
        // Rows 0..7 and 23..30 stay eight frame rows tall however tall the scroll; rows 8..22
        // stretch over what lies between.
        checkRect(quads[0].area, {40, 50, 350, 8});
        checkRect(quads[0].uv, topRows);
        checkRect(quads[1].area, {40, 58, 350, height - 16});
        checkRect(quads[1].uv, middleRows);
        checkRect(quads[2].area, {40, 50 + height - 8, 350, 8});
        checkRect(quads[2].uv, bottomRows);
        for (const Quad& quad : quads) {
            // Opaque, in the frame's units.
            CHECK(quad.color == Color::white());
            CHECK(quad.transform == LegendsN64Frame::transform());
        }
    }
    CHECK(LegendsN64Scroll::kTopCapRow + LegendsN64Scroll::kCapRows ==
          LegendsN64Scroll::kMiddleRow);
    CHECK(LegendsN64Scroll::kMiddleRow + LegendsN64Scroll::kMiddleRows ==
          LegendsN64Scroll::kBottomCapRow);

    // A scroll no taller than its two caps has nothing to stretch.
    const std::vector<Quad> caps = scrollOver(scroll, {0, 1, 70, 16});
    REQUIRE(caps.size() == 2);
    checkRect(caps[0].area, {0, 1, 70, 8});
    checkRect(caps[0].uv, topRows);
    checkRect(caps[1].area, {0, 9, 70, 8});
    checkRect(caps[1].uv, bottomRows);
}
