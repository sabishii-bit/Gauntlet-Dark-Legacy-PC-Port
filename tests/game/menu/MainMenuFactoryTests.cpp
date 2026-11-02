#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "game/menu/MainMenuFactory.h"
#include "game/widgets/OptionMenu.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** The retail title font's rows: 32 pixels tall at scale one. */
BitmapFont titleFont() {
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = ' '; c <= '~'; ++c) {
        glyphs.push_back({c, 16, 0, 0});
    }
    return BitmapFont::fromGlyphs(32, 8, std::move(glyphs));
}

TEST_CASE("the title menu's rows stand where Press Start stands and never leave the screen",
          "[game][menu][title][main-menu]") {
    // Two rows fit under the retail row; three climb just far enough to end above the
    // bottom; a column too tall for the room above the bottom shrinks instead of climbing
    // over the logo.
    CHECK(MainMenuFactory::place(2, 32, 384) == std::pair{304, 1.0f});
    CHECK(MainMenuFactory::place(3, 32, 384) == std::pair{280, 1.0f});
    const auto [top, scale] = MainMenuFactory::place(5, 32, 384);
    CHECK(top == MainMenuFactory::kHighestTop);
    CHECK(scale < 1.0f);
    CHECK(scale > 0.5f);
    CHECK(static_cast<f32>(top) + 5 * 32 * scale <= 384 - MainMenuFactory::kBottomMargin);
    CHECK(MainMenuFactory::place(0, 32, 384) == std::pair{304, 1.0f});
}

TEST_CASE("the title menu is built centred, every row in view, by the factory",
          "[game][menu][title][main-menu]") {
    const BitmapFont font = titleFont();
    test::FakeTexture sheet{64, 64};
    TextPainter painter;
    painter.setFont(&font, &sheet);
    const MenuScreen screen;
    const std::vector<MainMenuEntry> entries{{"START", 11}, {"OPTIONS", 12}, {"QUIT", 15, false}};
    const MenuDefinition definition = MainMenuFactory::build(entries, painter, screen);
    REQUIRE(definition.items.size() == 3);
    CHECK(definition.items[0].text == "START");
    CHECK(definition.items[2].code == 15);
    CHECK_FALSE(definition.items[2].enabled);
    CHECK(definition.x == -256); // each row centred on the screen's middle
    CHECK(definition.y == 280);
    CHECK(definition.scale == 1.0f);
    CHECK(definition.startSelects);
    CHECK(definition.title.empty());
    CHECK(definition.backdrop.empty());
    OptionMenu menu;
    menu.open(definition, painter, screen);
    for (usize i = 0; i < definition.items.size(); ++i) {
        CHECK(menu.itemY(i) + menu.lineHeight() <= screen.height - MainMenuFactory::kBottomMargin);
        CHECK(menu.itemY(i) >= MainMenuFactory::kHighestTop);
    }
    CHECK(menu.itemY(2) + menu.lineHeight() == screen.height - MainMenuFactory::kBottomMargin);
    // Rows registered later shrink the column before it could climb over the logo.
    std::vector<MainMenuEntry> more = entries;
    more.push_back({"NETPLAY", 13});
    more.push_back({"CREDITS", 16});
    more.push_back({"EXTRAS", 17});
    const MenuDefinition taller = MainMenuFactory::build(more, painter, screen);
    CHECK(taller.scale < 1.0f);
    CHECK(taller.cursorScale == taller.scale);
    menu.open(taller, painter, screen);
    CHECK(menu.itemY(0) >= MainMenuFactory::kHighestTop);
    CHECK(menu.itemY(5) + menu.lineHeight() <= screen.height - MainMenuFactory::kBottomMargin);
}

} // namespace
