#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "game/hud/CinematicBars.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a cut's bars reach the display's edges, over the frame's margins on a narrow one",
          "[game][screens][cutscene]") {
    // Wide (16:9), the frame itself, then narrower than it (4:3, portrait).
    const auto extent =
        GENERATE(Vec2{1920, 1080}, Vec2{640, 448}, Vec2{1920, 1440}, Vec2{600, 1000});
    CAPTURE(extent.x, extent.y);
    test::FakeRenderDevice device;
    Canvas canvas;
    const auto transform = makeVirtualScreenTransform(
        makeLetterboxProjection(640, 448, extent.x, extent.y), 512, 384, 640, 448);
    canvas.begin(device, transform);
    drawCinematicBars(canvas, 384);
    canvas.end();
    // The trigger cameras' bars end at rows 48 and 304 of the 384.
    const f32 topFoot = (transform * Vec4{0, 48, 0, 1}).y;
    const f32 bottomHead = (transform * Vec4{0, 304, 0, 1}).y;
    std::vector<Vec2> corners;
    for (const auto& draw : device.draws) {
        CHECK(draw.transform == Mat4{1});
        for (const auto& vertex : draw.vertices) {
            corners.emplace_back(vertex.position);
            CHECK(vertex.color == Color::black());
        }
    }
    REQUIRE_FALSE(corners.empty());
    const auto across = [](const Vec2& corner) { return corner.x; };
    const auto down = [](const Vec2& corner) { return corner.y; };
    CHECK(std::ranges::min(corners, {}, down).y == -1);
    CHECK(std::ranges::max(corners, {}, down).y == 1);
    CHECK(std::ranges::min(corners, {}, across).x == -1);
    CHECK(std::ranges::max(corners, {}, across).x == 1);
    // Between the bars the view stays open.
    constexpr f32 kEdge = 1e-5f;
    CHECK(std::ranges::all_of(corners, [&](const Vec2& corner) {
        return corner.y <= topFoot + kEdge || corner.y >= bottomHead - kEdge;
    }));
}

} // namespace
