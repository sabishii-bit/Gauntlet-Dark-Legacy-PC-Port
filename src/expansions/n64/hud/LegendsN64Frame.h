#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** The N64's 416x220 HUD frame on the 512x384 canvas, placed as the two consoles' video
 * hardware puts their pictures on a television. */
struct LegendsN64Frame {
    static constexpr f32 kWidth = 416;
    static constexpr f32 kHeight = 220;
    /** The canvas the frame is placed on, which the GameCube's 640x448 frame fills. */
    static constexpr f32 kCanvasWidth = 512;
    static constexpr f32 kCanvasHeight = 384;
    static constexpr f64 kViClockMHz = 48.681818 / 4; ///< NTSC video interface pixel clock
    static constexpr f64 kViColumns = 708 - 149; ///< game.bin FUN_e000f580: hStart for width 416
    static constexpr f64 kCubeClockMHz = 13.5;   ///< assumed: the GameCube's 640-pixel NTSC clock
    static constexpr f64 kCubeColumns = 640;     ///< the GameCube frame the canvas fills
    static constexpr f64 kCubeScanlines = 224;   ///< a field of that frame's 448 lines
    static constexpr f64 kScanline = kCanvasHeight / kCubeScanlines; ///< canvas rows a scanline
    static constexpr f64 kWindowScanlines = (498 - 60) / 2.0; ///< FUN_e000f580: vStart 60..498
    static constexpr f64 kRowsPerScanline = 1023.0 / 1024;    ///< FUN_e000f580: yScale 0x3ff (2.10)
    /** Scanlines from the canvas's top to the frame's. Assumed: both consoles centre their
     * 240-line pictures alike, so the window, 23 half-lines below the standard vStart of 37,
     * starts 5 lines below the GameCube's frame and ends with it. */
    static constexpr f64 kTopScanlines = 5;
    /** 1.1924 canvas units a column: the microseconds the video interface takes to draw one,
     * against those the GameCube takes for a canvas unit. */
    static constexpr f32 kColumn = static_cast<f32>((kViColumns / kViClockMHz / kWidth) /
                                                    (kCubeColumns / kCubeClockMHz / kCanvasWidth));
    static constexpr f32 kRow = static_cast<f32>(kScanline / kRowsPerScanline); ///< 1.7160
    static constexpr f32 kTop = static_cast<f32>(kTopScanlines * kScanline);    ///< 8.571
    static constexpr f32 kLeft = (kCanvasWidth - kWidth * kColumn) / 2;         ///< 7.973
    /** The frame rows the video interface shows, 218.79; the rest fall below the canvas, as
     * the N64 hides them. */
    static constexpr f64 kRowsShown = kWindowScanlines * kRowsPerScanline;

    static Vec2 toCanvas(Vec2 frame);
    static Vec2 toFrame(Vec2 canvas);
    /** Frame units to canvas units, for a CanvasFrame. */
    static Mat4 transform();
};

} // namespace gdl::game
