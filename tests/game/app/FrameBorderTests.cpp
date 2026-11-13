#include <cmath>

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

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr Vec2 kFrame{640, 448};

/** Draws what a screen leaves: the whole display covered. */
void drawScene(test::FakeRenderDevice& device) {
    ImmediateBatch scene;
    scene.rect({-1, -1, 2, 2}, 0.5f, Color::white());
    device.draw(scene, device.whiteTexture(), Mat4{1}, {});
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

} // namespace
