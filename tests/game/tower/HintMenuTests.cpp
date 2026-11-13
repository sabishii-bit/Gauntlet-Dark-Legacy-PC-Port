#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
#include "game/widgets/DialoguePresentation.h"

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

/** Three places to choose from, the first and last greyed. */
std::vector<MenuItem> choices() {
    std::vector<MenuItem> out{{"HOME", 0}, {"FIRST", 1}, {"SECOND", 2}};
    out[0].enabled = false;
    out[2].enabled = false;
    return out;
}

TEST_CASE("a topic answered with a page of choices greys some and burns once one is chosen",
          "[game][menu][hints][gateways]") {
    Fixture f;
    REQUIRE_FALSE(f.menu.choose(choices())); // no list of topics open to answer
    REQUIRE(f.menu.open(f.painter, MenuScreen{}, labels()));
    REQUIRE(f.menu.update(f.device, press(true, false, false), 2).kind ==
            HintMenuEvent::Kind::Moved);
    REQUIRE(f.menu.update(f.device, press(false, true, false), 2).kind ==
            HintMenuEvent::Kind::Asked);
    REQUIRE_FALSE(f.menu.choose({})); // nothing to offer
    REQUIRE(f.menu.choose(choices()));
    REQUIRE(f.menu.reading());
    // The list's own scroll and heading, the rows centred, the select prompt beneath.
    const OptionMenu& page = f.menu.page();
    CHECK(page.definition().title == "HELP");
    CHECK(page.definition().titleScale == HintMenu::kTitleScale);
    CHECK(page.definition().x == -256);
    CHECK(page.definition().selectLabel == "SELECT");
    CHECK(page.definition().playerLabel == "PLAYER");
    CHECK(page.definition().backdrop == "SCROLL_A");
    CHECK(page.backdropArea().x == 16.0f);
    CHECK_FALSE(page.definition().backdropFades); // the scroll is already out
    CHECK(page.definition().scale == 1.0f);       // short rows keep the topics' scale
    REQUIRE(page.definition().items.size() == 3);
    CHECK_FALSE(page.definition().items[0].enabled);
    CHECK(page.definition().items[1].enabled);
    CHECK_FALSE(page.definition().items[2].enabled);
    // The selection starts on the first that can be chosen and never rests on a greyed one.
    CHECK(page.selection() == 1);
    f.menu.update(f.device, press(true, false, false), 2);
    CHECK(page.selection() == 1);
    // Choosing it burns the scroll, the list under the page going with it.
    const HintMenuEvent chosen = f.menu.update(f.device, press(false, true, false), 2);
    CHECK(chosen.kind == HintMenuEvent::Kind::Chose);
    CHECK(chosen.topic == 1);
    CHECK(f.menu.burning());
    CHECK(f.menu.page().backdropReleased());
    CHECK_FALSE(f.menu.topics().isOpen());
    // While it burns nothing answers, and once burnt and faded the scroll is gone.
    CHECK(f.menu.update(f.device, press(false, true, false), 2).kind == HintMenuEvent::Kind::None);
    for (s32 i = 0; i < 40 && f.menu.active(); ++i) {
        f.menu.update(f.device, MenuInput{}, 2);
    }
    CHECK_FALSE(f.menu.active());
}

TEST_CASE("a page of choices keeps its widest row within the scroll's room, shrinking all rows",
          "[game][menu][hints][gateways]") {
    Fixture f;
    REQUIRE(f.menu.open(f.painter, MenuScreen{}, labels()));
    f.menu.update(f.device, press(false, true, false), 2);
    const std::string wide(60, 'W');
    REQUIRE(f.painter.measure(wide, 1.0f) > HintMenu::kChoicesRoom);
    REQUIRE(f.menu.choose({{"HOME", 0}, {wide, 1}}));
    const OptionMenu& page = f.menu.page();
    const f32 scale = page.definition().scale;
    CHECK(scale < 1.0f);
    CHECK(page.definition().cursorScale == scale); // the arrow shrinks with the rows
    // Written whole, never cut short, and every row on the scroll.
    CHECK(page.definition().items[1].text == wide);
    CHECK(f.painter.measure(wide, scale) <= HintMenu::kChoicesRoom);
    const Rect scroll = page.backdropArea();
    for (usize i = 0; i < page.definition().items.size(); ++i) {
        CAPTURE(i);
        const Rect area = page.itemArea(i);
        CHECK(area.width ==
              static_cast<f32>(f.painter.measure(page.definition().items[i].text, scale)));
        CHECK(area.x >= scroll.x);
        CHECK(area.x + area.width <= scroll.x + scroll.width);
    }
}

TEST_CASE("Back from a page of choices returns to the list where it was left",
          "[game][menu][hints][gateways]") {
    Fixture f;
    REQUIRE(f.menu.open(f.painter, MenuScreen{}, labels()));
    const MenuDefinition list = f.menu.topics().definition();
    f.menu.update(f.device, press(true, false, false), 2);
    REQUIRE(f.menu.update(f.device, press(false, true, false), 2).kind ==
            HintMenuEvent::Kind::Asked);
    REQUIRE(f.menu.choose(choices()));
    CHECK(f.menu.update(f.device, press(false, false, true), 2).kind ==
          HintMenuEvent::Kind::Returned);
    CHECK_FALSE(f.menu.reading());
    CHECK_FALSE(f.menu.burning());
    const OptionMenu& topics = f.menu.topics();
    REQUIRE(topics.isOpen());
    CHECK(topics.selection() == 1);
    CHECK(topics.definition().title == list.title);
    CHECK(topics.definition().x == list.x);
    CHECK(topics.definition().items.size() == list.items.size());
    CHECK_FALSE(topics.definition().backdropFades); // the scroll stays out
    // All greyed, nothing can be chosen; Back still returns.
    f.menu.update(f.device, press(false, true, false), 2);
    std::vector<MenuItem> greyed = choices();
    greyed[1].enabled = false;
    REQUIRE(f.menu.choose(greyed));
    CHECK(f.menu.update(f.device, press(false, true, false), 2).kind == HintMenuEvent::Kind::None);
    CHECK(f.menu.reading());
    CHECK(f.menu.update(f.device, press(false, false, true), 2).kind ==
          HintMenuEvent::Kind::Returned);
}

TEST_CASE("a page of choices takes the native look its topics opened in",
          "[game][menu][hints][gateways][imported-menu]") {
    Fixture f;
    /** Lays the topics out in its own column and draws its own backdrop and heading; a list of
     * choices takes the topics' layout unless it says otherwise. */
    class NativeLook final : public DialoguePresentation {
    public:
        explicit NativeLook(const TextPainter& text) : m_text(text) {}
        const TextPainter& text() const override { return m_text; }
        std::optional<MenuDefinition>
        nativeTopics(std::string_view /*title*/, std::span<const MenuItem> topics) const override {
            MenuDefinition definition;
            definition.backdrop = "native";
            definition.backdropX = 100;
            definition.backdropY = 60;
            definition.backdropWidth = 300;
            definition.backdropHeight = 200;
            definition.showCursor = false;
            definition.items.assign(topics.begin(), topics.end());
            for (usize i = 0; i < topics.size(); ++i) {
                definition.itemPositions.emplace_back(-256, 90 + static_cast<s32>(i) * 20);
            }
            return definition;
        }
        bool drawTopicsBackdrop(Canvas& /*canvas*/, const OptionMenu& menu,
                                std::string_view title) const override {
            drawn.emplace_back(&menu, std::string(title));
            return true;
        }
        f32 scale() const override { return 1; }
        s32 textWidth() const override { return 256; }
        usize linesPerPage() const override { return 8; }
        DialoguePage layout(std::string_view /*page*/, f32 /*scale*/,
                            std::string_view /*prompt*/) const override {
            return {};
        }
        void draw(Canvas& /*canvas*/, const DialoguePage& /*page*/, f32 /*scale*/,
                  std::string_view /*prompt*/, u8 /*promptAlpha*/) const override {}
        mutable std::vector<std::pair<const OptionMenu*, std::string>> drawn;

    private:
        const TextPainter& m_text;
    } native(f.painter);
    HintMenu menu;
    HintMenuArt art;
    art.textures.font = &f.sheet;
    art.textures.backdrop = &f.backdrop;
    art.presentation = &native;
    menu.setArt(std::move(art));
    HintMenuLabels words = labels();
    words.title = "SUMNER";
    REQUIRE(
        menu.open(f.painter, MenuScreen{}, words, native.nativeTopics(words.title, words.topics)));
    REQUIRE(menu.update(f.device, press(false, true, false), 2).kind == HintMenuEvent::Kind::Asked);
    REQUIRE(menu.choose(choices()));
    // Laid out as the look lays its topics out, its own heading drawn over the page.
    const OptionMenu& page = menu.page();
    CHECK(page.definition().backdrop == "native");
    CHECK(page.definition().title.empty());
    REQUIRE(page.definition().itemPositions.size() == 3);
    CHECK(page.definition().itemPositions[2] == Vec2(-256, 130));
    CHECK_FALSE(page.definition().items[0].enabled);
    Canvas canvas;
    canvas.begin(f.device, Mat4{1});
    menu.draw(canvas, f.painter);
    canvas.end();
    REQUIRE(native.drawn.size() == 1);
    CHECK(native.drawn.front().first == &page);
    CHECK(native.drawn.front().second == "SUMNER");
    // Back returns to the topics in the same look.
    CHECK(menu.update(f.device, press(false, false, true), 2).kind ==
          HintMenuEvent::Kind::Returned);
    CHECK(menu.topics().definition().backdrop == "native");
    CHECK(menu.topics().definition().itemPositions.size() == 2);
    // A choice closes it at once: a native scroll does not burn.
    REQUIRE(menu.update(f.device, press(false, true, false), 2).kind == HintMenuEvent::Kind::Asked);
    REQUIRE(menu.choose(choices()));
    const HintMenuEvent chosen = menu.update(f.device, press(false, true, false), 2);
    CHECK(chosen.kind == HintMenuEvent::Kind::Chose);
    CHECK(chosen.topic == 1);
    CHECK_FALSE(menu.burning());
    CHECK_FALSE(menu.active());
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
