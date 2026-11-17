#include <bit>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"

namespace {

using namespace gdl;
using Catch::Approx;

/** Whether two transforms agree bit for bit, -0.0 and +0.0 apart. */
bool sameBits(const Mat4& a, const Mat4& b) {
    constexpr s32 kSide = 4;
    for (s32 column = 0; column < kSide; ++column) {
        for (s32 row = 0; row < kSide; ++row) {
            if (std::bit_cast<u32>(a[column][row]) != std::bit_cast<u32>(b[column][row])) {
                return false;
            }
        }
    }
    return true;
}

/** A game's own frame on the canvas: moved, and scaled by more down than across. */
Mat4 gameFrame() {
    return glm::scale(glm::translate(Mat4{1.0f}, Vec3{8, 9, 0}), Vec3{1.25f, 1.5f, 1});
}

/** Where `placed` puts a point of its own space in clip space. */
Vec2 clipOf(const Mat4& placed, f32 x, f32 y) {
    const Vec4 point = placed * Vec4{x, y, 0, 1};
    return Vec2{point.x / point.w, point.y / point.w};
}

TEST_CASE("canvas overlay state applies to glyph batches and resets on the next begin",
          "[ui][canvas][build-label]") {
    test::FakeRenderDevice device;
    Canvas canvas;
    DrawState overlay;
    overlay.depthWrite = false;
    overlay.depthTest = false;
    canvas.begin(device, Mat4{1}, overlay);
    canvas.fill({0, 0, 8, 8}, Color::white());
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    CHECK_FALSE(device.draws[0].state.depthTest);
    CHECK_FALSE(device.draws[0].state.depthWrite);
    canvas.begin(device, Mat4{1});
    canvas.fill({0, 0, 8, 8}, Color::white());
    canvas.end();
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[1].state.depthTest);
    CHECK(device.draws[1].state.depthWrite);
}
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

TEST_CASE("masking above and below a frame covers only the rows a narrower display leaves",
          "[ui][canvas][frame-border]") {
    // 640x448 exactly, wide (16:9, 21:9), then narrower than the frame (4:3, 5:4, portrait).
    const auto extent = GENERATE(Extent2D{640, 448}, Extent2D{1920, 1080}, Extent2D{3440, 1440},
                                 Extent2D{1920, 1440}, Extent2D{1280, 1024}, Extent2D{600, 1000});
    CAPTURE(extent.width, extent.height);
    test::FakeRenderDevice device;
    Canvas canvas;
    const auto projection = makeLetterboxProjection(640, 448, static_cast<f32>(extent.width),
                                                    static_cast<f32>(extent.height));
    canvas.begin(device, projection);
    canvas.maskAboveAndBelow(0, 448);
    canvas.end();
    const f32 top = (projection * Vec4{0, 0, 0, 1}).y;
    const f32 bottom = (projection * Vec4{0, 448, 0, 1}).y;
    if (extent.width * 448 >= extent.height * 640) {
        CHECK(device.draws.empty()); // the frame fills the display's height
        return;
    }
    // The GameCube's 640x448 frame in its 480-line picture: sixteen of its 480 lines on each
    // side, 48 of 1440 rows here.
    if (extent.width == 1920 && extent.height == 1440) {
        CHECK(top == Approx(-1 + 96.0f / 1440));
        CHECK(bottom == Approx(1 - 96.0f / 1440));
    }
    REQUIRE(device.draws.size() == 1);
    const auto& draw = device.draws.front();
    CHECK(draw.transform == Mat4{1});
    CHECK_FALSE(draw.state.depthTest);
    CHECK_FALSE(draw.state.depthWrite);
    f32 area = 0;
    for (usize i = 0; i + 2 < draw.vertices.size(); i += 3) {
        const Vec2 a{draw.vertices[i].position};
        const Vec2 b{draw.vertices[i + 1].position};
        const Vec2 c{draw.vertices[i + 2].position};
        const Vec2 middle = (a + b + c) / 3.0f;
        CHECK((middle.y < top || middle.y > bottom)); // the frame itself stays open
        area += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
        CHECK(draw.vertices[i].color == Color::black());
    }
    CHECK(test::minCorner(draw) == Vec2{-1, -1});
    CHECK(test::maxCorner(draw) == Vec2{1, 1});
    CHECK(area == Approx(2 * ((top + 1) + (1 - bottom))));
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

TEST_CASE("a local frame applies before the canvas transform and goes with its scope",
          "[engine][ui][canvas]") {
    test::FakeRenderDevice device;
    const test::FakeTexture sprite{4, 4};
    const Mat4 transform = makeScreenProjection(512, 384);
    const Mat4 outer = gameFrame();
    const Mat4 inner = glm::translate(Mat4{1.0f}, Vec3{20, 30, 0});
    const Rect area{0, 0, 4, 4};
    Canvas canvas;
    canvas.begin(device, transform);
    canvas.draw(sprite, area);
    {
        const CanvasFrame frame(canvas, outer);
        CHECK(canvas.localTransform() == outer);
        canvas.draw(sprite, area);
        {
            const CanvasFrame nested(canvas, inner);
            CHECK(canvas.localTransform() == outer * inner);
            canvas.draw(sprite, area);
        }
        CHECK(canvas.localTransform() == outer);
        canvas.draw(sprite, area);
    }
    CHECK(canvas.localTransform() == Mat4{1.0f});
    canvas.draw(sprite, area);
    canvas.end();
    // One texture throughout, yet each change of frame starts a draw of its own, and every
    // sprite keeps its own corners for its transform to place.
    REQUIRE(device.draws.size() == 5);
    CHECK(device.draws[0].transform == transform);
    CHECK(device.draws[1].transform == transform * outer);
    CHECK(device.draws[2].transform == transform * (outer * inner));
    CHECK(device.draws[3].transform == transform * outer);
    CHECK(device.draws[4].transform == transform);
    for (const auto& draw : device.draws) {
        CHECK(draw.texture == &sprite);
        CHECK(test::minCorner(draw) == Vec2{0, 0});
        CHECK(test::maxCorner(draw) == Vec2{4, 4});
    }

    // A triangle and a prepared batch take the frame too, the batch's own transform first.
    device.draws.clear();
    ImmediateBatch batch;
    batch.rect(area, 0.5f, Color::white());
    const Mat4 batchLocal = glm::translate(Mat4{1.0f}, Vec3{2, 3, 0});
    canvas.begin(device, transform);
    {
        const CanvasFrame frame(canvas, outer);
        canvas.fillTriangle({0, 0}, {4, 0}, {0, 4}, Color::white());
        canvas.submit(batch, sprite, batchLocal);
    }
    canvas.end();
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].transform == transform * outer);
    CHECK(Vec2(device.draws[0].vertices[1].position) == Vec2{4, 0});
    CHECK(device.draws[1].transform == transform * outer * batchLocal);

    // A local transform left set goes with the canvas's end, and the next begin starts with none.
    device.draws.clear();
    canvas.begin(device, transform);
    canvas.setLocalTransform(outer);
    canvas.draw(sprite, area);
    canvas.end();
    CHECK(canvas.localTransform() == Mat4{1.0f});
    canvas.begin(device, transform);
    CHECK(canvas.localTransform() == Mat4{1.0f});
    canvas.draw(sprite, area);
    canvas.end();
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].transform == transform * outer);
    CHECK(device.draws[1].transform == transform);
}

TEST_CASE("masks and bands keep to the local frame", "[engine][ui][canvas]") {
    test::FakeRenderDevice device;
    Canvas canvas;
    // A wide display, so that the masks have margins beside the canvas to reach.
    const Mat4 transform = makeVirtualScreenTransform(makeLetterboxProjection(640, 448, 1280, 720),
                                                      512, 384, 640, 448);
    const Mat4 placed = transform * gameFrame();
    canvas.begin(device, transform);
    {
        const CanvasFrame frame(canvas, gameFrame());
        canvas.maskOutside({0, 0, 320, 200});
        canvas.fillHorizontalBand(0, 20, Color::black());
        canvas.maskAboveAndBelow(40, 160);
        canvas.fillScreen(Color::white());
    }
    canvas.end();
    REQUIRE(device.draws.size() == 4);
    for (const auto& draw : device.draws) {
        CHECK(draw.transform == Mat4{1});
        CHECK_FALSE(draw.state.depthTest);
        CHECK_FALSE(draw.state.depthWrite);
    }

    // The mask leaves open the frame's area, where the frame puts it on the canvas.
    const Vec2 low = clipOf(placed, 0, 0);
    const Vec2 high = clipOf(placed, 320, 200);
    const auto& mask = device.draws[0];
    f32 masked = 0;
    for (usize i = 0; i + 2 < mask.vertices.size(); i += 3) {
        const Vec2 a{mask.vertices[i].position};
        const Vec2 b{mask.vertices[i + 1].position};
        const Vec2 c{mask.vertices[i + 2].position};
        const Vec2 middle = (a + b + c) / 3.0f;
        CHECK((middle.x <= low.x || middle.x >= high.x || middle.y <= low.y || middle.y >= high.y));
        masked += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
    }
    CHECK(masked == Approx(4 - (high.x - low.x) * (high.y - low.y)));

    // The band runs the display's width over the frame's rows 0 to 20.
    const auto& band = device.draws[1];
    CHECK(test::minCorner(band).x == Approx(-1));
    CHECK(test::maxCorner(band).x == Approx(1));
    CHECK(test::minCorner(band).y == Approx(clipOf(placed, 0, 0).y));
    CHECK(test::maxCorner(band).y == Approx(clipOf(placed, 0, 20).y));

    // The rows above the frame's row 40 and below its row 160 are masked out to the edges.
    const f32 first = clipOf(placed, 0, 40).y;
    const f32 last = clipOf(placed, 0, 160).y;
    const auto& rows = device.draws[2];
    f32 covered = 0;
    for (usize i = 0; i + 2 < rows.vertices.size(); i += 3) {
        const Vec2 a{rows.vertices[i].position};
        const Vec2 b{rows.vertices[i + 1].position};
        const Vec2 c{rows.vertices[i + 2].position};
        const Vec2 middle = (a + b + c) / 3.0f;
        CHECK((middle.y < first || middle.y > last));
        covered += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
    }
    CHECK(test::minCorner(rows) == Vec2{-1, -1});
    CHECK(test::maxCorner(rows) == Vec2{1, 1});
    CHECK(covered == Approx(2 * ((first + 1) + (1 - last))));

    // A screen fill still covers the whole viewport.
    CHECK(test::minCorner(device.draws[3]) == Vec2{-1, -1});
    CHECK(test::maxCorner(device.draws[3]) == Vec2{1, 1});
}

TEST_CASE("without a local frame every draw keeps the canvas's own transform, bit for bit",
          "[engine][ui][canvas]") {
    // The projection's depth offset is -0.0, which a product with the identity turns to +0.0.
    const Mat4 transform = makeScreenProjection(512, 384);
    REQUIRE_FALSE(sameBits(transform * Mat4{1.0f}, transform));
    const Mat4 local = glm::translate(Mat4{1.0f}, Vec3{3, 4, 0});
    const Rect area{0, 0, 4, 4};
    ImmediateBatch batch;
    batch.rect(area, 0.5f, Color::white());
    test::FakeRenderDevice device;
    const test::FakeTexture sprite{4, 4};
    Canvas canvas;
    canvas.begin(device, transform);
    canvas.draw(sprite, area);
    canvas.fillTriangle({0, 0}, {4, 0}, {0, 4}, Color::white());
    canvas.submit(batch, sprite, local);
    {
        const CanvasFrame frame(canvas, local);
        canvas.draw(sprite, area);
    }
    // Once the frame has gone there is none again, as there is once the identity is set outright.
    canvas.draw(sprite, area);
    canvas.fillTriangle({0, 0}, {4, 0}, {0, 4}, Color::white());
    canvas.submit(batch, sprite, local);
    canvas.setLocalTransform(local);
    canvas.draw(sprite, area);
    canvas.setLocalTransform(Mat4{1.0f});
    canvas.draw(sprite, area);
    canvas.end();
    REQUIRE(device.draws.size() == 9);
    CHECK(sameBits(device.draws[0].transform, transform));
    CHECK(sameBits(device.draws[1].transform, transform));
    CHECK(sameBits(device.draws[2].transform, transform * local));
    CHECK(device.draws[3].transform == transform * local);
    CHECK(sameBits(device.draws[4].transform, transform));
    CHECK(sameBits(device.draws[5].transform, transform));
    CHECK(sameBits(device.draws[6].transform, transform * local));
    CHECK(device.draws[7].transform == transform * local);
    CHECK(sameBits(device.draws[8].transform, transform));
}

} // namespace
