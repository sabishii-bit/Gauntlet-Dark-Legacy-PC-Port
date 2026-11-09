#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/render/Image.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "game/tower/HintMenu.h"

namespace {

using namespace gdl;
using namespace gdl::game;

BitmapFont wideFont() {
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = 'A'; c <= 'Z'; ++c) {
        glyphs.push_back({c, 8, 0, 0});
    }
    return BitmapFont::fromGlyphs(10, 4, std::move(glyphs));
}

HintMenuLabels labels() {
    HintMenuLabels out;
    out.title = "HELP";
    out.topics = {{"GENERAL", 39}, {"GUARDIANS", 40}};
    out.select = "SELECT";
    out.player = "PLAYER";
    return out;
}

MenuInput press(bool down, bool select, bool back) {
    MenuInput input;
    input.down = down;
    input.select = select;
    input.back = back;
    return input;
}

struct Fixture {
    BitmapFont font = wideFont();
    test::FakeTexture sheet{64, 64};
    test::FakeTexture backdrop{16, 16};
    test::FakeTexture ring{8, 8};
    Image scroll = Image::filled(8, 8, Color::white());
    Image mask = Image::filled(4, 4, Color::rgba(0, 0, 0, 0));
    TextPainter painter;
    test::FakeRenderDevice device;
    HintMenu menu;

    Fixture() {
        painter.setFont(&font, &sheet);
        HintMenuArt art;
        art.textures.font = &sheet;
        art.textures.backdrop = &backdrop;
        art.scroll = &scroll;
        art.burnMasks.assign(BurnDialogueScroll::kFrameCount, &mask);
        art.burnRing.assign(BurnDialogueScroll::kFrameCount, &ring);
        menu.setArt(std::move(art));
    }
};

TEST_CASE("the hint scroll lists its topics, answers one with a page and returns",
          "[game][menu][hints]") {
    Fixture f;
    REQUIRE_FALSE(f.menu.active());
    REQUIRE_FALSE(f.menu.open(TextPainter{}, MenuScreen{}, labels())); // nothing to write with
    REQUIRE_FALSE(f.menu.open(f.painter, MenuScreen{}, HintMenuLabels{}));
    REQUIRE(f.menu.open(f.painter, MenuScreen{}, labels()));
    REQUIRE(f.menu.active());
    REQUIRE(f.menu.topics().definition().title == "HELP");
    REQUIRE(f.menu.topics().definition().x == HintMenu::kTopicsX);
    REQUIRE(f.menu.topics().backdropArea().x == 16.0f);
    REQUIRE(f.menu.topics().backdropArea().y == 8.0f);

    REQUIRE(f.menu.update(f.device, press(true, false, false), 2).kind ==
            HintMenuEvent::Kind::Moved);
    const HintMenuEvent asked = f.menu.update(f.device, press(false, true, false), 2);
    REQUIRE(asked.kind == HintMenuEvent::Kind::Asked);
    REQUIRE(asked.topic == 40);
    REQUIRE_FALSE(f.menu.reading());

    f.menu.read(f.painter, "THE LICH", {"ONE\nTWO", "THREE"}, 0.8f, false, 16);
    REQUIRE(f.menu.reading());
    const MenuDefinition& page = f.menu.page().definition();
    REQUIRE(page.title == "THE LICH");
    REQUIRE(page.body.size() == 2);
    REQUIRE(page.bodyScale == HintMenu::kPageScale * 0.8f);
    REQUIRE(f.menu.page().bodyTop() == HintMenu::kPageTop);
    REQUIRE(page.selectLabel.empty()); // a page only goes back
    REQUIRE_FALSE(page.backdropFades);
    // Select does nothing on a page; Back returns to the list where it was.
    REQUIRE(f.menu.update(f.device, press(false, true, false), 2).kind ==
            HintMenuEvent::Kind::None);
    REQUIRE(f.menu.update(f.device, press(false, false, true), 2).kind ==
            HintMenuEvent::Kind::Returned);
    REQUIRE_FALSE(f.menu.reading());
    REQUIRE(f.menu.topics().selection() == 1);

    // A lone centred passage sits about the middle of the screen.
    f.menu.read(f.painter, "A HINT", {"ONE\nTWO"}, 1.0f, true, 0);
    const s32 lineHeight = f.painter.lineHeight(HintMenu::kPageScale);
    REQUIRE(f.menu.page().bodyTop() == 192 - lineHeight);
}

TEST_CASE("a hint menu draws with the font borrowed when its native topics opened",
          "[game][menu][hints][imported-menu]") {
    Fixture f;
    REQUIRE(f.menu.open(f.painter, MenuScreen{}, labels()));
    f.menu.update(f.device, {}, 120);
    Canvas canvas;
    canvas.begin(f.device, Mat4{1});
    f.menu.draw(canvas, TextPainter{});
    canvas.end();
    CHECK(std::ranges::any_of(f.device.draws,
                              [&](const auto& draw) { return draw.texture == &f.sheet; }));
}

TEST_CASE("closing a hint menu releases its borrowed painter before the level is destroyed",
          "[game][menu][hints][imported-menu]") {
    HintMenu menu;
    HintMenu::LocalSnapshot closed;
    NativeStateDigest before;
    {
        const auto font = wideFont();
        test::FakeTexture sheet{64, 64};
        TextPainter painter;
        painter.setFont(&font, &sheet);
        REQUIRE(menu.open(painter, MenuScreen{}, labels()));
        menu.read(painter, "HELP", {"GENERAL"}, 1, true, 0);
        menu.close();
        CHECK_FALSE(menu.active());
        CHECK_FALSE(menu.topics().isOpen());
        CHECK_FALSE(menu.page().isOpen());
        REQUIRE(menu.appendNativeDigest(before));
        REQUIRE(before.checkedFinish());
        // Releasing the former owner's font must not change a closed menu's
        // state. Check while it is alive so a stale borrow fails without UB.
        painter.setFont(nullptr, nullptr);
        NativeStateDigest released;
        REQUIRE(menu.appendNativeDigest(released));
        REQUIRE(released.checkedFinish() == before.checkedFinish());
        menu.saveLocal(closed);
    }

    NativeStateDigest after;
    REQUIRE(menu.appendNativeDigest(after));
    REQUIRE(after.checkedFinish());
    CHECK(after.checkedFinish() == before.checkedFinish());
    NativeStateDigest saved;
    REQUIRE(menu.appendLocalDigest(saved, closed));
    REQUIRE(saved.checkedFinish());
    CHECK(saved.checkedFinish() == before.checkedFinish());

    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    menu.draw(canvas, TextPainter{});
    canvas.end();
    CHECK(device.draws.empty());
}

TEST_CASE("backing out of the topics burns the scroll away", "[game][menu][hints]") {
    Fixture f;
    REQUIRE(f.menu.open(f.painter, MenuScreen{}, labels()));
    REQUIRE(f.menu.update(f.device, press(false, false, true), 2).kind ==
            HintMenuEvent::Kind::Left);
    REQUIRE(f.menu.burning());
    REQUIRE(f.menu.topics().backdropReleased());
    // While it burns nothing answers, and once burnt and faded the scroll is gone.
    REQUIRE(f.menu.update(f.device, press(false, true, false), 2).kind ==
            HintMenuEvent::Kind::None);
    for (s32 i = 0; i < 40 && f.menu.active(); ++i) {
        f.menu.update(f.device, MenuInput{}, 2);
    }
    REQUIRE_FALSE(f.menu.active());

    // Without the burn frames it simply fades.
    HintMenu plain;
    REQUIRE(plain.open(f.painter, MenuScreen{}, labels()));
    REQUIRE(plain.update(f.device, press(false, false, true), 2).kind == HintMenuEvent::Kind::Left);
    REQUIRE_FALSE(plain.burning());
    plain.close();
    REQUIRE_FALSE(plain.active());
}

TEST_CASE("the hint scroll draws its page's passages in ink on the scroll", "[game][menu][hints]") {
    Fixture f;
    REQUIRE(f.menu.open(f.painter, MenuScreen{}, labels()));
    f.menu.update(f.device, press(false, true, false), 2);
    f.menu.read(f.painter, "T", {"AB\nC"}, 1.0f, false, 0);
    Canvas canvas;
    canvas.begin(f.device, Mat4{1.0f});
    f.menu.draw(canvas, f.painter);
    canvas.end();
    REQUIRE_FALSE(f.device.draws.empty());
    REQUIRE(f.device.draws.front().texture == &f.backdrop);
    bool ink = false;
    for (const test::RecordedDraw& draw : f.device.draws) {
        for (const ImmediateVertex& vertex : draw.vertices) {
            ink = ink || (vertex.color.r == HintMenu::kInk.r &&
                          vertex.color.g == HintMenu::kInk.g && vertex.color.b == HintMenu::kInk.b);
        }
    }
    REQUIRE(ink);
}

TEST_CASE("hint menu rewind restores revisions semantic topics pages and burn clocks",
          "[game][menu][hints][hint-menu-local-rewind]") {
    for (s32 at = 0; at < 70; ++at) {
        for (s32 delay = 1; delay <= 8; ++delay) {
            CAPTURE(at, delay);
            Fixture expected;
            Fixture corrected;
            const auto tick = [](Fixture& f, s32 t) {
                t %= 60;
                if (t == 0) {
                    REQUIRE(f.menu.open(f.painter, MenuScreen{}, labels()));
                }
                MenuInput input;
                input.back = t == 8 || t == 10;
                const u8 topic = t == 3 ? 2 : 0;
                const auto event = f.menu.update(f.device, input, 1, topic, f.menu.revision());
                if (event.kind == HintMenuEvent::Kind::Asked) {
                    f.menu.read(f.painter, "THE LICH", {"ONE", "TWO"}, 0.8f, false, 16);
                }
                return event;
            };
            for (s32 t = 0; t < at; ++t) {
                tick(expected, t);
                tick(corrected, t);
            }
            HintMenu::LocalSnapshot saved;
            corrected.menu.saveLocal(saved);
            REQUIRE_FALSE(expected.menu.restoreLocal(saved));
            std::vector<HintMenuEvent> events;
            for (s32 t = at; t < at + delay; ++t) {
                events.push_back(tick(expected, t));
                corrected.menu.close();
                REQUIRE(corrected.menu.open(corrected.painter, MenuScreen{}, labels()));
                corrected.menu.read(corrected.painter, "WRONG", {"THREE"}, 1, true, 0);
                corrected.menu.update(corrected.device, press(false, false, true), 10);
                corrected.menu.update(corrected.device, press(false, false, true), 10);
                corrected.menu.prepare(corrected.device);
            }
            const auto created = corrected.device.texturesCreated;
            const auto uploaded = corrected.device.textureUpdates;
            REQUIRE(corrected.menu.restoreLocal(saved));
            for (s32 t = at; t < at + delay; ++t) {
                const auto event = tick(corrected, t);
                CHECK(event.kind == events[static_cast<usize>(t - at)].kind);
                CHECK(event.topic == events[static_cast<usize>(t - at)].topic);
            }
            CHECK(corrected.device.texturesCreated == created);
            CHECK(corrected.device.textureUpdates == uploaded);
            CHECK(corrected.menu.revision() == expected.menu.revision());
            CHECK(corrected.menu.active() == expected.menu.active());
            CHECK(corrected.menu.reading() == expected.menu.reading());
            CHECK(corrected.menu.burning() == expected.menu.burning());
            CHECK(corrected.menu.topics().selection() == expected.menu.topics().selection());
            CHECK(corrected.menu.topics().backdropReleased() ==
                  expected.menu.topics().backdropReleased());
            CHECK(corrected.menu.page().definition().body ==
                  expected.menu.page().definition().body);
            CHECK(corrected.menu.acceptsTopic(2, corrected.menu.revision()) ==
                  expected.menu.acceptsTopic(2, expected.menu.revision()));
            const auto draw = [](Fixture& f) {
                f.menu.prepare(f.device);
                f.device.draws.clear();
                Canvas canvas;
                canvas.begin(f.device, Mat4{1});
                f.menu.draw(canvas, f.painter);
                canvas.end();
            };
            draw(expected);
            draw(corrected);
            REQUIRE(corrected.device.draws.size() == expected.device.draws.size());
            for (usize i = 0; i < expected.device.draws.size(); ++i) {
                test::checkRecordedDraw(corrected.device.draws[i], expected.device.draws[i]);
            }
            corrected.menu.setArt({});
            CHECK_FALSE(corrected.menu.restoreLocal(saved));
        }
    }
}

} // namespace
