#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/render/Image.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "game/menu/BurnDialogueScroll.h"

namespace {

using namespace gdl;
using namespace gdl::game;

constexpr Color kOpaque = Color::rgba(200, 150, 100, 255);
constexpr Color kClear = Color::rgba(0, 0, 0, 0);

/** A 2x2 mask that keeps every texel except the listed holes. */
Image maskWithHoles(const std::vector<std::pair<u32, u32>>& holes) {
    Image mask = Image::filled(2, 2, Color::rgba(255, 255, 255, 255));
    for (const auto& [x, y] : holes) {
        mask.setPixel(x, y, kClear);
    }
    return mask;
}

struct Fixture {
    test::FakeRenderDevice device;
    Image scroll = Image::filled(4, 4, kOpaque);
    Image frame0 = maskWithHoles({{1, 1}});
    Image frame1 = maskWithHoles({{1, 0}, {1, 1}});
    Image frame2 = maskWithHoles({{1, 0}, {0, 1}, {1, 1}});
    test::FakeTexture ring0{2, 2};
    test::FakeTexture ring1{2, 2};
    BurnDialogueScroll fire;

    bool start() {
        return fire.start(device, Rect{10.0f, 20.0f, 40.0f, 40.0f}, scroll,
                          {&frame0, &frame1, &frame2}, {&ring0, &ring1});
    }
};

TEST_CASE("the scroll is cut out wherever the mask has burnt through", "[game][fire]") {
    Fixture f;
    REQUIRE(f.start());
    REQUIRE(f.fire.active());
    REQUIRE(f.fire.frame() == 0);
    REQUIRE(f.device.texturesCreated == 1);
    // Frame 0 has burnt the bottom-right quarter. The mask is filtered over the scroll, so
    // only texels with no intact mask nearby are gone.
    REQUIRE(f.fire.compositeAlpha(3, 3) == 0);
    REQUIRE(f.fire.compositeAlpha(2, 2) == 255);
    REQUIRE(f.fire.compositeAlpha(0, 0) == 255);

    f.fire.step(BurnDialogueScroll::kTicksPerFrame);
    REQUIRE(f.fire.frame() == 1);
    REQUIRE(f.fire.compositeAlpha(3, 0) == 0);
    REQUIRE(f.fire.compositeAlpha(3, 3) == 0);
    REQUIRE(f.fire.compositeAlpha(2, 0) == 255);
    REQUIRE(f.fire.compositeAlpha(0, 3) == 255);

    f.fire.step(BurnDialogueScroll::kTicksPerFrame);
    REQUIRE(f.fire.compositeAlpha(0, 3) == 0);
    REQUIRE(f.fire.compositeAlpha(3, 0) == 0);
    REQUIRE(f.fire.compositeAlpha(0, 0) == 255);
    REQUIRE(f.fire.compositeAlpha(1, 1) == 255);
}

TEST_CASE("the effect uploads once per frame and ends after the last one", "[game][fire]") {
    Fixture f;
    REQUIRE(f.start());
    f.fire.prepare(f.device);
    REQUIRE(f.device.textureUpdates == 1);
    f.fire.prepare(f.device);
    REQUIRE(f.device.textureUpdates == 1);

    f.fire.step(1);
    f.fire.prepare(f.device);
    REQUIRE(f.device.textureUpdates == 1);
    f.fire.step(1);
    f.fire.prepare(f.device);
    REQUIRE(f.device.textureUpdates == 2);

    f.fire.step(BurnDialogueScroll::kTicksPerFrame * (BurnDialogueScroll::kFrameCount - 1));
    REQUIRE_FALSE(f.fire.active());
    f.fire.step(1);
    REQUIRE_FALSE(f.fire.active());
}

TEST_CASE("drawing covers the area with the scroll and the ring frame", "[game][fire]") {
    Fixture f;
    REQUIRE(f.start());
    Canvas canvas;
    canvas.begin(f.device, Mat4{1.0f});
    f.fire.draw(canvas);
    canvas.end();
    REQUIRE(f.device.draws.size() == 2);
    REQUIRE(f.device.draws[1].texture == &f.ring0);
    REQUIRE(test::maxCorner(f.device.draws[0]).x == 50.0f);
    REQUIRE(test::maxCorner(f.device.draws[0]).y == 60.0f);

    f.fire.step(BurnDialogueScroll::kTicksPerFrame * 5);
    f.device.draws.clear();
    canvas.begin(f.device, Mat4{1.0f});
    f.fire.draw(canvas);
    canvas.end();
    REQUIRE(f.device.draws[1].texture == &f.ring1);

    f.fire.reset();
    REQUIRE_FALSE(f.fire.active());
    f.device.draws.clear();
    canvas.begin(f.device, Mat4{1.0f});
    f.fire.draw(canvas);
    canvas.end();
    REQUIRE(f.device.draws.empty());
}

TEST_CASE("starting without frames or with mismatched masks fails", "[game][fire]") {
    Fixture f;
    REQUIRE_FALSE(f.fire.start(f.device, Rect{}, f.scroll, {}, {&f.ring0}));
    REQUIRE_FALSE(f.fire.start(f.device, Rect{}, f.scroll, {&f.frame0}, {}));
    const Image odd = Image::filled(3, 3, kClear);
    REQUIRE_FALSE(f.fire.start(f.device, Rect{}, f.scroll, {&f.frame0, &odd}, {&f.ring0}));
    REQUIRE_FALSE(f.fire.active());
}

} // namespace
