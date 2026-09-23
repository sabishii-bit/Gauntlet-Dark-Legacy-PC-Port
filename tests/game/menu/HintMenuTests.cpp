#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/render/Image.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "game/menu/HintMenu.h"

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
    out.back = "BACK";
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

} // namespace
