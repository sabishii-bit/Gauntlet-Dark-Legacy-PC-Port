#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/render/Image.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "game/widgets/BurnDialogueScroll.h"

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
    REQUIRE(f.device.texturesCreated == 0);
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
    REQUIRE(f.device.texturesCreated == 1);
    REQUIRE(f.device.textureUpdates == 0);
    f.fire.prepare(f.device);
    REQUIRE(f.device.textureUpdates == 0);

    f.fire.step(1);
    f.fire.prepare(f.device);
    REQUIRE(f.device.textureUpdates == 0);
    f.fire.step(1);
    f.fire.prepare(f.device);
    REQUIRE(f.device.textureUpdates == 1);

    f.fire.step(BurnDialogueScroll::kTicksPerFrame * (BurnDialogueScroll::kFrameCount - 1));
    REQUIRE_FALSE(f.fire.active());
    f.fire.step(1);
    REQUIRE_FALSE(f.fire.active());
}

TEST_CASE("drawing covers the area with the scroll and the ring frame", "[game][fire]") {
    Fixture f;
    REQUIRE(f.start());
    f.fire.prepare(f.device);
    Canvas canvas;
    canvas.begin(f.device, Mat4{1.0f});
    f.fire.draw(canvas);
    canvas.end();
    REQUIRE(f.device.draws.size() == 2);
    REQUIRE(f.device.draws[1].texture == &f.ring0);
    REQUIRE(test::maxCorner(f.device.draws[0]).x == 50.0f);
    REQUIRE(test::maxCorner(f.device.draws[0]).y == 60.0f);

    f.fire.step(BurnDialogueScroll::kTicksPerFrame * 5);
    f.fire.prepare(f.device);
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
    REQUIRE_FALSE(f.fire.start(f.device, Rect{}, f.scroll, {nullptr}, {&f.ring0}));
    REQUIRE_FALSE(f.fire.start(f.device, Rect{}, f.scroll, {&f.frame0, nullptr}, {&f.ring0}));
    REQUIRE_FALSE(f.fire.active());
}

TEST_CASE("missing flame frames preserve the burn timer and rewind without GPU work",
          "[game][fire][burn-scroll-local-rewind]") {
    Fixture f;
    const std::vector<const Image*> masks{&f.frame0, &f.frame1, &f.frame2};
    const std::vector<const Texture*> ring{nullptr, &f.ring1, nullptr};
    REQUIRE_FALSE(BurnDialogueScroll::ready(&f.scroll, masks, ring));
    REQUIRE(f.fire.start(f.device, Rect{10, 20, 40, 40}, f.scroll, masks, ring));
    REQUIRE(f.fire.active());
    CHECK(f.device.texturesCreated == 0);
    CHECK(f.device.textureUpdates == 0);
    const auto draw = [&] {
        f.fire.prepare(f.device);
        f.device.draws.clear();
        Canvas canvas;
        canvas.begin(f.device, Mat4{1});
        f.fire.draw(canvas);
        canvas.end();
    };
    draw();
    REQUIRE(f.device.draws.size() == 1);
    CHECK(f.fire.compositeAlpha(3, 3) == 0);
    REQUIRE(f.device.texturesCreated == 1);
    REQUIRE(f.device.textureUpdates == 0);

    f.fire.step(BurnDialogueScroll::kTicksPerFrame);
    CHECK(f.device.textureUpdates == 0);
    draw();
    REQUIRE(f.device.draws.size() == 2);
    CHECK(f.device.draws[1].texture == &f.ring1);
    BurnDialogueScroll::LocalSnapshot saved;
    f.fire.saveLocal(saved);

    // Exhaust a wrong branch, including its render cache, then restore the
    // valid flame frame. Rewind itself must not upload or create a texture.
    f.fire.step(BurnDialogueScroll::kTicksPerFrame);
    draw();
    REQUIRE(f.device.draws.size() == 1);
    f.fire.step(BurnDialogueScroll::kFrameCount * BurnDialogueScroll::kTicksPerFrame);
    REQUIRE_FALSE(f.fire.active());
    const auto created = f.device.texturesCreated;
    const auto updated = f.device.textureUpdates;
    REQUIRE(f.fire.restoreLocal(saved));
    CHECK(f.device.texturesCreated == created);
    CHECK(f.device.textureUpdates == updated);
    REQUIRE(f.fire.active());
    CHECK(f.fire.frame() == 1);
    draw();
    REQUIRE(f.device.draws.size() == 2);
    CHECK(f.device.draws[1].texture == &f.ring1);

    for (s32 frame = 2; frame < BurnDialogueScroll::kFrameCount; ++frame) {
        CAPTURE(frame);
        f.fire.step(BurnDialogueScroll::kTicksPerFrame);
        REQUIRE(f.fire.active());
        REQUIRE(f.fire.frame() == frame);
        draw();
        REQUIRE(f.device.draws.size() == 1);
        CHECK(f.fire.compositeAlpha(0, 3) == 0);
    }
    f.fire.step(BurnDialogueScroll::kTicksPerFrame);
    REQUIRE_FALSE(f.fire.active());
    draw();
    CHECK(f.device.draws.empty());
}

TEST_CASE("an empty burn mask preserves the uncut scroll fallback", "[game][fire]") {
    Fixture f;
    const Image emptyMask;
    REQUIRE(f.fire.start(f.device, Rect{10, 20, 40, 40}, f.scroll, {&emptyMask}, {nullptr}));
    CHECK(f.fire.compositeAlpha(3, 3) == kOpaque.a);
    CHECK(f.device.texturesCreated == 0);
    f.fire.prepare(f.device);
    REQUIRE(f.device.texturesCreated == 1);
    Canvas canvas;
    canvas.begin(f.device, Mat4{1});
    f.fire.draw(canvas);
    canvas.end();
    REQUIRE(f.device.draws.size() == 1);
}

TEST_CASE("burn rewind restores frame births and completion without touching GPU caches",
          "[game][fire][burn-scroll-local-rewind]") {
    for (s32 at = 0; at < 50; ++at) {
        for (s32 delay = 1; delay <= 8; ++delay) {
            CAPTURE(at, delay);
            Fixture expected;
            Fixture corrected;
            const auto tick = [](Fixture& f, s32 tickNumber) {
                if (tickNumber == 3) {
                    REQUIRE(f.start());
                } else {
                    f.fire.step(1);
                }
            };
            for (s32 t = 0; t < at; ++t) {
                tick(expected, t);
                tick(corrected, t);
            }
            BurnDialogueScroll::LocalSnapshot saved;
            corrected.fire.saveLocal(saved);
            REQUIRE_FALSE(expected.fire.canRestoreLocal(saved));
            for (s32 t = at; t < at + delay; ++t) {
                tick(expected, t);
                corrected.fire.reset();
                REQUIRE(corrected.start());
                corrected.fire.step(t % 3 + 20);
                corrected.fire.prepare(corrected.device);
            }
            const auto created = corrected.device.texturesCreated;
            const auto uploaded = corrected.device.textureUpdates;
            REQUIRE(corrected.fire.restoreLocal(saved));
            for (s32 t = at; t < at + delay; ++t) {
                tick(corrected, t);
            }
            CHECK(corrected.device.texturesCreated == created);
            CHECK(corrected.device.textureUpdates == uploaded);
            CHECK(corrected.fire.active() == expected.fire.active());
            CHECK(corrected.fire.frame() == expected.fire.frame());
            if (expected.fire.active()) {
                for (u32 y = 0; y < 4; ++y) {
                    for (u32 x = 0; x < 4; ++x) {
                        CHECK(corrected.fire.compositeAlpha(x, y) ==
                              expected.fire.compositeAlpha(x, y));
                    }
                }
            }
            const auto draw = [](Fixture& f) {
                f.fire.prepare(f.device);
                f.device.draws.clear();
                Canvas canvas;
                canvas.begin(f.device, Mat4{1});
                f.fire.draw(canvas);
                canvas.end();
            };
            draw(expected);
            draw(corrected);
            REQUIRE(corrected.device.draws.size() == expected.device.draws.size());
            for (usize i = 0; i < expected.device.draws.size(); ++i) {
                test::checkRecordedDraw(corrected.device.draws[i], expected.device.draws[i]);
            }
        }
    }
    BurnDialogueScroll empty;
    CHECK_FALSE(empty.restoreLocal(BurnDialogueScroll::LocalSnapshot{}));
}

} // namespace
