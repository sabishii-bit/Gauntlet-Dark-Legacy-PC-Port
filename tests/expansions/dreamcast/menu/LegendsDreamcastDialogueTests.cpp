#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/menu/LegendsDreamcastDialogue.h"
#include "expansions/dreamcast/menu/LegendsDreamcastEntrance.h"
#include "game/hud/LevelMessages.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "fonts/font32.fnt")) {
        SKIP("Native Dreamcast menu assets are not installed");
    }
    return root;
}
} // namespace

TEST_CASE("Dreamcast entry title retains native timing", "[dreamcast-dialogue]") {
    CHECK(LegendsDreamcastEntrance::scale(-1) == 0);
    CHECK(LegendsDreamcastEntrance::scale(0) == Catch::Approx(.025f));
    CHECK(LegendsDreamcastEntrance::scale(1) == Catch::Approx(.05f));
    CHECK(LegendsDreamcastEntrance::scale(40) == Catch::Approx(1.025f));
    CHECK(LegendsDreamcastEntrance::scale(79) == 2);
    CHECK(LegendsDreamcastEntrance::scale(90) == 2);
    CHECK(LegendsDreamcastEntrance::scale(91) == 0);
    CHECK(LegendsDreamcastEntrance::scale(100000) == 0);
}

TEST_CASE("Absent Dreamcast menu assets do not bind GameCube artwork", "[dreamcast-dialogue]") {
    test::FakeRenderDevice device;
    LegendsDreamcastDialogue style;
    CHECK_FALSE(style.load(device, test::dataDirectory() / "no-dreamcast-assets"));
    CHECK_FALSE(style.text().ready());
    CHECK_FALSE(style.menuText().ready());
    CHECK(style.menuTextures()->backdrop == nullptr);
    CHECK(device.texturesCreated == 0);
}

TEST_CASE("Dreamcast scroll pages and menus use their own native font atlases",
          "[dreamcast-dialogue][assets]") {
    const auto root = nativeRoot();
    test::FakeRenderDevice device;
    LegendsDreamcastDialogue style;
    REQUIRE(style.load(device, root));
    REQUIRE(style.text().font());
    REQUIRE(style.menuText().font());
    CHECK(style.text().font()->height() == 8);
    CHECK(style.text().font()->spaceWidth() == 8);
    CHECK(style.menuText().font()->height() == 32);
    CHECK(style.menuText().font()->spaceWidth() == 16);
    REQUIRE(style.menuTextures());
    const auto& art = *style.menuTextures();
    REQUIRE(art.backdrop);
    CHECK(art.backdrop->width() == 256);
    CHECK(art.backdrop->height() == 256);
    CHECK(art.font == nullptr);
    CHECK(art.glow == nullptr);
    CHECK(art.parchment == nullptr);
    CHECK(art.garamond[0] == nullptr);
    CHECK(art.burn[0] == nullptr);
    CHECK(art.icon == nullptr);

    LevelMessages messages;
    messages.setPresentation(&style);
    const std::array<std::string, 1> prose{"THE TOWER IS YOUR SAFE HAVEN."};
    REQUIRE(messages.openImported(device, prose, nullptr));
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    messages.draw(canvas);
    canvas.end();
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws.front().texture == art.backdrop);
    CHECK(device.draws.front().vertices.size() == 6);
    CHECK(device.draws.back().texture->width() == 128);
    CHECK(device.draws.back().texture->height() == 128);
    CHECK(device.draws.back().vertices.front().color == Color::rgba(0, 31, 0));
    const auto hold = messages.step(ScrollBox::kHoldTicks, 1);
    CHECK_FALSE(hold.stopVoice);
    CHECK(messages.active());
    const auto dismiss = messages.step(1, 1);
    CHECK_FALSE(dismiss.burnSound);
    CHECK_FALSE(messages.active());

    const std::array<std::string, 1> longText{std::string(2000, 'W')};
    REQUIRE(messages.openImported(device, longText, nullptr));
    CHECK(messages.scroll().pageCount() > 1);
    for (const auto& line : messages.scroll().lines()) {
        CHECK(style.text().measure(line, style.scale()) <= style.textWidth());
    }
    CHECK(messages.scroll().area().y >= 0);
    CHECK(messages.scroll().area().bottom() <= 320);
    const auto fullPage = style.layout("A\nB\nC\nD\nE\nF\nG\nH\nI\nJ", style.scale(), "Continue");
    CHECK(fullPage.promptY + style.text().lineHeight(style.scale()) < fullPage.area.bottom());

    const std::array choices{MenuItem{"History", 0}, MenuItem{"Obelisk Hint", 1},
                             MenuItem{"Rune Hint", 2}, MenuItem{"Enemy Hint", 3},
                             MenuItem{"exit", 4}};
    auto topics = style.nativeTopics({}, choices);
    REQUIRE(topics);
    CHECK(topics->title.empty());
    CHECK(topics->backdropHeight == 150);
    CHECK(topics->backdropWidth < 300);
    CHECK(topics->backdropY == 85);
    REQUIRE(topics->itemPositions.size() == 5);
    CHECK(topics->itemPositions[0] == Vec2(-256, 109));
    CHECK(topics->itemPositions[3] == Vec2(-256, 169));
    CHECK(topics->itemPositions[4].y == 199);
    CHECK(topics->itemPositions[4].x > 256);
    OptionMenu menu;
    menu.open(*topics, style.text(), {});
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    menu.draw(canvas, style.text(), art);
    canvas.end();
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws.front().texture == art.backdrop);
    CHECK(device.draws.back().texture->width() == 128);
    CHECK(device.draws.back().texture->height() == 128);
    for (usize choice = 0; choice < choices.size(); ++choice) {
        MenuInput click;
        const Rect area = menu.itemArea(choice);
        click.pointer = Vec2(area.x + area.width * .5f, area.y + area.height * .5f);
        click.pointerPressed = true;
        const auto event = menu.update(click, 1);
        CHECK(event.action == MenuAction::Choice);
        CHECK(event.code == static_cast<s32>(choice));
    }

    device.draws.clear();
    canvas.begin(device, Mat4{1});
    LegendsDreamcastEntrance::draw(canvas, style.menuText(), "Tower", 512, 79);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().texture->width() == 256);
    CHECK(device.draws.front().texture->height() == 256);
    CHECK(device.draws.front().vertices.front().color == Color::white());
    CHECK(device.draws.front().vertices.front().position.y == Catch::Approx(17));

    CHECK_FALSE(style.load(device, test::dataDirectory() / "no-dreamcast-assets"));
    CHECK_FALSE(style.text().ready());
    CHECK_FALSE(style.menuText().ready());
    CHECK(style.menuTextures()->backdrop == nullptr);
}

TEST_CASE("Dreamcast entrance display names come from LEVL instead of archive names",
          "[dreamcast-dialogue][assets]") {
    const auto root = nativeRoot();
    const auto file = root / formats::dreamcastLegendsWorldDataFile("levelt1");
    if (!std::filesystem::is_regular_file(file)) {
        SKIP("Native Dreamcast world metadata is not installed");
    }
    const auto scope = formats::dreamcastLegendsFixtureScope(readFile(file), "levelt1");
    REQUIRE(scope);
    CHECK(scope->title == "Tower");
}
