#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/render/Image.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "game/widgets/ScrollBox.h"

namespace {

using namespace gdl;
using namespace gdl::game;

BitmapFont sampleFont() {
    return BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
}

TEST_CASE("pages split at line breaks and drop a final empty line", "[game][menu][scroll]") {
    REQUIRE(ScrollBox::splitLines("a\nb\n") == std::vector<std::string>{"a", "b"});
    REQUIRE(ScrollBox::splitLines("a\n\nb") == std::vector<std::string>{"a", "", "b"});
    REQUIRE(ScrollBox::splitLines("") == std::vector<std::string>{""});
    REQUIRE(ScrollBox::splitLines("\n") == std::vector<std::string>{""});
}

TEST_CASE("a scroll sizes itself to each page and turns pages on a button after a hold",
          "[game][menu][scroll]") {
    const BitmapFont font = sampleFont();
    const test::FakeTexture sheet{32, 16};
    TextPainter painter;
    painter.setFont(&font, &sheet);
    test::FakeRenderDevice device;
    const test::FakeTexture backdrop{64, 64};
    const test::FakeTexture button{20, 20};
    ScrollBoxArt art;
    art.backdrop = &backdrop;
    art.button = &button;
    ScrollBox box;
    box.setText(&painter);
    box.setArt(art);
    REQUIRE_FALSE(box.active());
    REQUIRE_FALSE(box.open(device, {}, 1.0f, "AB"));

    REQUIRE(box.open(device, {"AB\nA\n", "B"}, 1.0f, "AB"));
    REQUIRE(box.active());
    REQUIRE(box.pageCount() == 2);
    REQUIRE(box.page() == 0);
    REQUIRE(box.lines() == std::vector<std::string>{"AB", "A"});
    // Two lines of ten with four between, the widest fourteen wide: the scroll adds 96 both
    // ways and sits centred on (256, 160).
    REQUIRE(box.area() == Rect{201.0f, 100.0f, 110.0f, 120.0f});

    // A button does nothing until the page has been up fifteen ticks.
    box.step(14, 1);
    REQUIRE(box.page() == 0);
    box.step(1, 0);
    box.step(1, 1);
    REQUIRE(box.page() == 1);
    REQUIRE(box.lines() == std::vector<std::string>{"B"});
    REQUIRE(box.area() == Rect{204.0f, 107.0f, 104.0f, 106.0f});

    // The scroll, the text and prompt, and the button icon draw in that order, the text in
    // the parchment's dark ink.
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    box.draw(canvas);
    canvas.end();
    REQUIRE(device.draws.size() >= 3);
    REQUIRE(device.draws[1].vertices[0].color == ScrollBox::kTextColor);
    REQUIRE(device.draws[0].texture == &backdrop);
    REQUIRE(test::minCorner(device.draws[0]) == Vec2{204.0f, 107.0f});
    REQUIRE(test::maxCorner(device.draws[0]) == Vec2{308.0f, 213.0f});
    REQUIRE(device.draws[1].texture == &sheet);
    REQUIRE(device.draws.back().texture == &button);
    REQUIRE(test::minCorner(device.draws.back()) == Vec2{190.0f, 107.0f + 32.0f + 10.0f + 8.0f});

    // On the last page the button closes the scroll; without burn art it goes at once.
    box.step(15, 0);
    box.step(1, 2);
    REQUIRE_FALSE(box.active());
    REQUIRE_FALSE(box.burning());

    // Without a font there is nothing to draw with.
    ScrollBox bare;
    REQUIRE_FALSE(bare.open(device, {"A"}, 1.0f, "A"));
}

TEST_CASE("the words go before the scroll burns", "[game][menu][scroll]") {
    const BitmapFont font = sampleFont();
    const test::FakeTexture sheet{32, 16};
    TextPainter painter;
    painter.setFont(&font, &sheet);
    test::FakeRenderDevice device;
    const test::FakeTexture backdrop{64, 64};
    const Image backdropImage = Image::filled(4, 4, Color::rgba(200, 180, 120, 255));
    const Image halfBurnt = Image::filled(2, 2, Color::rgba(255, 255, 255, 255));
    const Image allBurnt = Image::filled(2, 2, Color::rgba(255, 255, 255, 0));
    const test::FakeTexture ring{2, 2};
    ScrollBoxArt art;
    art.backdrop = &backdrop;
    art.backdropImage = &backdropImage;
    art.burnMasks = {&halfBurnt, &allBurnt};
    art.burnRing = {&ring};
    ScrollBox box;
    box.setText(&painter);
    box.setArt(art);
    REQUIRE(box.open(device, {"AB"}, 1.0f, "AB"));
    box.step(15, 0);
    box.step(1, 1);
    REQUIRE(box.active());
    REQUIRE(box.burning());
    // While it burns only the fire shows: no sheet of words, no whole backdrop.
    box.prepare(device);
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    box.draw(canvas);
    canvas.end();
    REQUIRE_FALSE(device.draws.empty());
    for (const auto& draw : device.draws) {
        REQUIRE(draw.texture != &sheet);
        REQUIRE(draw.texture != &backdrop);
    }
    for (s32 i = 0; i < 200 && box.burning(); ++i) {
        box.step(1, 0);
    }
    REQUIRE_FALSE(box.burning());
    REQUIRE_FALSE(box.active());
}

TEST_CASE("a narrow page still leaves room for the prompt", "[game][menu][scroll]") {
    const BitmapFont font = sampleFont();
    const test::FakeTexture sheet{32, 16};
    TextPainter painter;
    painter.setFont(&font, &sheet);
    test::FakeRenderDevice device;
    ScrollBox box;
    box.setText(&painter);
    // The prompt at half scale is 32 wide plus 32 of scroll; the page's single glyph would
    // give only 6 plus 96.
    REQUIRE(box.open(device, {"A"}, 1.0f, "ABABABAB"));
    REQUIRE(box.area().width == 102.0f);
    REQUIRE(box.area().x == 205.0f);
}

TEST_CASE("scroll rewind preserves page holds pulse and burn through corrected button ticks",
          "[game][menu][scroll][scroll-box-local-rewind]") {
    const BitmapFont font = sampleFont();
    const test::FakeTexture sheet{32, 16};
    const test::FakeTexture ring{4, 4};
    const Image image = Image::filled(4, 4, Color::white());
    const Image mask = Image::filled(2, 2, Color::rgba(255, 255, 255, 0));
    TextPainter text;
    text.setFont(&font, &sheet);
    ScrollBoxArt art;
    art.backdrop = &sheet;
    art.backdropImage = &image;
    art.burnMasks = {&mask};
    art.burnRing = {&ring};
    for (s32 at = 0; at < 90; ++at) {
        for (s32 delay = 1; delay <= 8; ++delay) {
            CAPTURE(at, delay);
            test::FakeRenderDevice device;
            ScrollBox expected;
            ScrollBox corrected;
            for (auto* box : {&expected, &corrected}) {
                box->setText(&text);
                box->setArt(art);
            }
            const auto tick = [&](ScrollBox& box, s32 t) {
                if (t == 1) {
                    REQUIRE(box.open(device, {"A", "BB"}, 0.7f, "AB"));
                } else {
                    box.step(1, t % 3 == 0 ? 4U : 0U);
                }
            };
            for (s32 t = 0; t < at; ++t) {
                tick(expected, t);
                tick(corrected, t);
            }
            ScrollBox::LocalSnapshot saved;
            corrected.saveLocal(saved);
            REQUIRE_FALSE(expected.restoreLocal(saved));
            for (s32 t = at; t < at + delay; ++t) {
                tick(expected, t);
                if (t % 2 == 0) {
                    corrected.close();
                    REQUIRE(corrected.open(device, {"B"}, 1.2f, "AA"));
                }
                corrected.step(20, 1);
            }
            const auto created = device.texturesCreated;
            const auto uploaded = device.textureUpdates;
            REQUIRE(corrected.restoreLocal(saved));
            for (s32 t = at; t < at + delay; ++t) {
                tick(corrected, t);
            }
            CHECK(device.texturesCreated == created);
            CHECK(device.textureUpdates == uploaded);
            CHECK(corrected.active() == expected.active());
            CHECK(corrected.page() == expected.page());
            CHECK(corrected.pageCount() == expected.pageCount());
            CHECK(corrected.area() == expected.area());
            CHECK(corrected.lines() == expected.lines());
            CHECK(corrected.promptAlpha() == expected.promptAlpha());
            CHECK(corrected.burnFrame() == expected.burnFrame());
            const auto draw = [&](ScrollBox& box) {
                box.prepare(device);
                device.draws.clear();
                Canvas canvas;
                canvas.begin(device, Mat4{1});
                box.draw(canvas);
                canvas.end();
                return device.draws;
            };
            const auto a = draw(expected);
            const auto b = draw(corrected);
            REQUIRE(a.size() == b.size());
            for (usize i = 0; i < a.size(); ++i) {
                test::checkRecordedDraw(b[i], a[i]);
            }
            corrected.setArt(art);
            CHECK_FALSE(corrected.restoreLocal(saved));
        }
    }
}

} // namespace
