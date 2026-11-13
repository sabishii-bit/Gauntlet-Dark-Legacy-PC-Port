#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "game/app/FrameBorder.h"
#include "game/hud/CinematicBars.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr Vec2 kFrame{640, 448};
/** The 2D canvas a scene draws on, fitted to the frame. */
constexpr Vec2 kCanvas{512, 384};
constexpr Color kScene = Color::white(); ///< what a screen leaves over the whole display

/** Draws what a screen leaves: the whole display covered. */
void drawScene(test::FakeRenderDevice& device) {
    ImmediateBatch scene;
    scene.rect({-1, -1, 2, 2}, 0.5f, kScene);
    device.draw(scene, device.whiteTexture(), Mat4{1}, {});
}

/** The colour a display shows at the centre of its pixel (`column`, `row`) once `draws` are
 * drawn in order: the last one over it, as nothing drawn over the scene tests depth. */
std::optional<Color> shownAt(std::span<const test::RecordedDraw> draws, Extent2D display,
                             u32 column, u32 row) {
    const Vec2 centre{((static_cast<f32>(column) + 0.5f) / static_cast<f32>(display.width) * 2) - 1,
                      ((static_cast<f32>(row) + 0.5f) / static_cast<f32>(display.height) * 2) - 1};
    std::optional<Color> shown;
    for (const auto& draw : draws) {
        for (usize i = 0; i + 2 < draw.vertices.size(); i += 3) {
            std::array<Vec2, 3> corner{};
            for (usize k = 0; k < corner.size(); ++k) {
                const Vec4 clip = draw.transform * Vec4{draw.vertices[i + k].position, 1};
                corner[k] = Vec2{clip} / clip.w;
            }
            // Which side of each edge the centre lies on; inside, all agree with the winding.
            std::array<f32, 3> side{};
            for (usize k = 0; k < corner.size(); ++k) {
                const Vec2 edge = corner[(k + 1) % corner.size()] - corner[k];
                const Vec2 toCentre = centre - corner[k];
                side[k] = (edge.x * toCentre.y) - (edge.y * toCentre.x);
            }
            const f32 winding = side[0] + side[1] + side[2]; // twice the triangle's signed area
            if (winding != 0 &&
                std::ranges::all_of(side, [&](f32 turn) { return turn * winding >= 0; })) {
                shown = draw.vertices[i].color;
            }
        }
    }
    return shown;
}

/** How many of a display's rows show black across it from its top edge down and from its
 * bottom edge up, and whether every row between them shows the scene. */
struct ShownRows {
    u32 blackAbove = 0;
    u32 blackBelow = 0;
    bool sceneBetween = true;
};

/** Reads a display's rows at its left and right edges and its middle. */
ShownRows shownRows(std::span<const test::RecordedDraw> draws, Extent2D display) {
    const std::array columns{0U, display.width / 2, display.width - 1};
    const auto rowShows = [&](u32 row, Color color) {
        return std::ranges::all_of(
            columns, [&](u32 column) { return shownAt(draws, display, column, row) == color; });
    };
    ShownRows rows;
    while (rows.blackAbove < display.height && rowShows(rows.blackAbove, Color::black())) {
        ++rows.blackAbove;
    }
    while (rows.blackAbove + rows.blackBelow < display.height &&
           rowShows(display.height - 1 - rows.blackBelow, Color::black())) {
        ++rows.blackBelow;
    }
    for (u32 row = rows.blackAbove; row + rows.blackBelow < display.height; ++row) {
        rows.sceneBetween = rows.sceneBetween && rowShows(row, kScene);
    }
    return rows;
}

TEST_CASE("the frame's border blacks out the rows a narrower display leaves, over the scene",
          "[game][frame-border]") {
    struct Shape {
        Extent2D display;
        f32 margin = 0; ///< display rows above the frame and below it
    };
    // 4:3, where the GameCube's border is 16 of its picture's 480 lines a side: 48 of 1440 rows.
    // Then 5:4 and portrait.
    const auto shape =
        GENERATE(Shape{{1920, 1440}, 48}, Shape{{1280, 1024}, 64}, Shape{{600, 1000}, 290});
    CAPTURE(shape.display.width, shape.display.height);
    test::FakeRenderDevice device;
    Canvas canvas;
    drawScene(device);
    drawFrameBorder(canvas, device, kFrame, shape.display);
    REQUIRE(device.draws.size() == 2);
    const auto& border = device.draws.back();
    CHECK(border.transform == Mat4{1});
    CHECK_FALSE(border.state.depthTest); // over the scene, whatever depth it left
    CHECK_FALSE(border.state.depthWrite);
    const auto height = static_cast<f32>(shape.display.height);
    const auto rowOf = [&](f32 clipY) { return (clipY + 1) / 2 * height; };
    f32 area = 0;
    for (usize i = 0; i + 2 < border.vertices.size(); i += 3) {
        const Vec2 a{border.vertices[i].position};
        const Vec2 b{border.vertices[i + 1].position};
        const Vec2 c{border.vertices[i + 2].position};
        const f32 row = rowOf((a.y + b.y + c.y) / 3);
        CHECK((row < shape.margin || row > height - shape.margin)); // the frame stays open
        area += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
        CHECK(border.vertices[i].color == Color::black());
    }
    // Both bands run the display's whole width, out to its top and bottom edges.
    CHECK(test::minCorner(border) == Vec2{-1, -1});
    CHECK(test::maxCorner(border) == Vec2{1, 1});
    CHECK(area == Approx(2 * 2 * (2 * shape.margin / height)));
}

TEST_CASE("the frame's border leaves a display as wide as the frame or wider alone",
          "[game][frame-border]") {
    // The frame itself, 16:9 and 21:9: the frame fills the display's height.
    const auto display = GENERATE(Extent2D{640, 448}, Extent2D{1920, 1080}, Extent2D{3440, 1440});
    CAPTURE(display.width, display.height);
    test::FakeRenderDevice device;
    Canvas canvas;
    drawScene(device);
    drawFrameBorder(canvas, device, kFrame, display);
    CHECK(device.draws.size() == 1);
}

TEST_CASE("a display is black from its top and bottom edges to a cut's bars, and to the frame",
          "[game][frame-border][cutscene]") {
    struct Shape {
        Extent2D display;
        u32 border = 0;    ///< rows black above the frame and below it in play
        u32 topBar = 0;    ///< rows black from the top edge to the foot of a cut's top bar
        u32 bottomBar = 0; ///< rows black from the head of its bottom bar to the bottom edge
    };
    // 4:3 at two sizes and 5:4, narrower than the frame, then 16:10 and 16:9. The bars are rows
    // 0..48 and 304..384 of the 384 the scene's canvas has (TriggerCamUpdate's two blits at
    // 0x8001BD34 and 0x8001BD64), and the black runs on to the display's edges as the GameCube's
    // video border does beyond its frame: at 1920x1440, 48 rows a side, then 168 and 280.
    const auto shape = GENERATE(Shape{{1920, 1440}, 48, 216, 328}, Shape{{1024, 768}, 26, 115, 175},
                                Shape{{1280, 1024}, 64, 176, 251}, Shape{{1920, 1200}, 0, 150, 250},
                                Shape{{1920, 1080}, 0, 135, 225});
    const Extent2D display = shape.display;
    CAPTURE(display.width, display.height);
    test::FakeRenderDevice device;
    Canvas canvas;
    drawScene(device);
    const auto overScene = [&] {
        return std::ranges::none_of(
            std::span{device.draws}.subspan(1),
            [](const test::RecordedDraw& draw) { return draw.state.depthTest; });
    };
    SECTION("in play") {
        drawFrameBorder(canvas, device, kFrame, display);
        CHECK(overScene());
        const ShownRows rows = shownRows(device.draws, display);
        CHECK(rows.blackAbove == shape.border);
        CHECK(rows.blackBelow == shape.border);
        CHECK(rows.sceneBetween); // beside the frame a wide display shows the view
    }
    SECTION("during a cut") {
        // On the scene's canvas, as it draws them, then the frame's border after every screen.
        canvas.begin(
            device, makeVirtualScreenTransform(
                        makeLetterboxProjection(kFrame.x, kFrame.y, static_cast<f32>(display.width),
                                                static_cast<f32>(display.height)),
                        kCanvas.x, kCanvas.y, kFrame.x, kFrame.y));
        drawCinematicBars(canvas, kCanvas.y);
        canvas.end();
        const usize bars = device.draws.size();
        drawFrameBorder(canvas, device, kFrame, display);
        CHECK(overScene());
        // The bars reach the display's edges themselves; the border leaves them as they are.
        for (const usize drawn : {bars, device.draws.size()}) {
            CAPTURE(drawn);
            const ShownRows rows = shownRows(std::span{device.draws}.first(drawn), display);
            CHECK(rows.blackAbove == shape.topBar);
            CHECK(rows.blackBelow == shape.bottomBar);
            CHECK(rows.sceneBetween);
        }
    }
}

} // namespace
