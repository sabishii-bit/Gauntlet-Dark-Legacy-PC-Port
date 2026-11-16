#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "expansions/n64/hud/LegendsN64Frame.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Frame = LegendsN64Frame;

constexpr f64 kHundredth = 0.01;
/** A sprite's depth on the canvas, which the frame leaves alone. */
constexpr f32 kSpriteDepth = 0.5f;

/** A figure given rounded to `step`: the value lies within half a step of it. */
Approx rounded(f64 figure, f64 step) {
    return Approx(figure).epsilon(0).margin(step / 2);
}

/** Where a point mapped there and back again, or by the matrix, lands: on `value`, but for
 * float rounding. */
Approx within(f32 value) {
    constexpr f64 kRounding = 1e-3;
    return Approx(value).epsilon(0).margin(kRounding);
}

TEST_CASE("the N64 frame lands on the canvas as the video hardware places it",
          "[legends][n64-hud][n64-frame]") {
    // A column is 559 of the video interface's 12.17 MHz pixels over the frame's 416, against
    // the GameCube's 640 at 13.5 MHz over the canvas's 512. A row takes 1024/1023 of a
    // scanline (yScale 0x3ff), and the canvas's 384 rows hold 224 scanlines. Neither factor
    // is the uniform 512/416, and the rows are not the 220 fitted into the window's 219 lines
    // (1.7065).
    CHECK(Frame::kColumn == rounded(1.1924, 0.0001));
    CHECK(Frame::kRow == rounded(1.7160, 0.0001));

    const Vec2 origin = Frame::toCanvas({0, 0});
    CHECK(origin.x == rounded(7.97, kHundredth));
    CHECK(origin.y == rounded(8.57, kHundredth));
    const Vec2 corner = Frame::toCanvas({Frame::kWidth, Frame::kHeight});
    CHECK(corner.x == rounded(504.03, kHundredth));
    CHECK(corner.y == rounded(386.08, kHundredth));
    CHECK(Frame::toCanvas({Frame::kWidth / 2, 0}).x == within(Frame::kCanvasWidth / 2));

    // The window shows rows up to 218.79, the last of them on the canvas's last row, and the
    // frame's last row falls below the canvas.
    CHECK(Frame::kRowsShown == rounded(218.79, kHundredth));
    CHECK(Frame::toCanvas({0, static_cast<f32>(Frame::kRowsShown)}).y ==
          rounded(384.00, kHundredth));
    CHECK(Frame::toCanvas({0, Frame::kHeight - 1}).y > Frame::kCanvasHeight);

    // The panels' strip starts at row 170, a panel is 104 by 50, and row 1 is where the N64
    // hangs its boss meters (Boss.bin FUN_e08887b8).
    CHECK(Frame::toCanvas({0, 170}).y == rounded(300.28, kHundredth));
    const Vec2 panel = Frame::toCanvas({104, 50}) - origin;
    CHECK(panel.x == rounded(124.01, kHundredth));
    CHECK(panel.y == rounded(85.80, kHundredth));
    CHECK(Frame::toCanvas({0, 1}).y == rounded(10.29, kHundredth));

    // toFrame undoes toCanvas, and the matrix places a point where toCanvas does, leaving its
    // depth and weight alone, so that a CanvasFrame can draw in frame units.
    const Mat4 transform = Frame::transform();
    const std::array framePoints{Vec2{0, 0}, Vec2{Frame::kWidth / 2, 110},
                                 Vec2{Frame::kWidth, Frame::kHeight}, Vec2{12.5f, 170},
                                 Vec2{-20, -4}};
    for (const Vec2 point : framePoints) {
        CAPTURE(point.x, point.y);
        const Vec2 canvas = Frame::toCanvas(point);
        const Vec2 back = Frame::toFrame(canvas);
        CHECK(back.x == within(point.x));
        CHECK(back.y == within(point.y));
        const Vec4 placed = transform * Vec4{point, kSpriteDepth, 1};
        CHECK(placed.x == within(canvas.x));
        CHECK(placed.y == within(canvas.y));
        CHECK(placed.z == kSpriteDepth);
        CHECK(placed.w == 1.0f);
    }
    const std::array canvasPoints{Vec2{0, 0},
                                  Vec2{Frame::kCanvasWidth / 2, Frame::kCanvasHeight / 2},
                                  Vec2{Frame::kCanvasWidth, Frame::kCanvasHeight}};
    for (const Vec2 point : canvasPoints) {
        CAPTURE(point.x, point.y);
        const Vec2 there = Frame::toCanvas(Frame::toFrame(point));
        CHECK(there.x == within(point.x));
        CHECK(there.y == within(point.y));
    }
}

} // namespace
