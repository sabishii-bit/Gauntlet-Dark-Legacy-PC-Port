#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PauseMenu.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("pause menu refuses absent artwork or a player outside the party", "[pause]") {
    test::FakeRenderDevice device;
    PauseMenu menu;
    GameContext context;
    context.unpackedRoot = test::scratchDirectory("pause-empty");
    const std::array party{PartyMember{}};
    CHECK_FALSE(menu.open(device, context, party, 3));
    CHECK_FALSE(menu.open(device, context, party, 0));
    CHECK_FALSE(menu.isOpen());
}
TEST_CASE("pause menus save then load without mutating the live party", "[pause][unpacked]") {
    test::FakeRenderDevice device;
    GameConfig config;
    config.save.directory = test::scratchDirectory("pause-files").string();
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = test::unpackedOrSkip("STATIC/textures.json").parent_path().parent_path();
    bool settingsSaved = false;
    context.saveSettings = [&](const GameConfig& next) {
        config = next;
        settingsSaved = true;
        return true;
    };
    std::array party{PartyMember{}};
    party[0].player = 2;
    party[0].save.name = "PAUSE";
    party[0].save.gold = 123;
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 2));
    CHECK(menu.player() == 2);
    MenuInput down;
    down.down = true;
    MenuInput select;
    select.select = true;
    select.start = true; // Enter is both menu-select and menu-start by default.
    MenuInput back;
    back.back = true;
    const auto step = [&](const MenuInput& input) { return menu.update(1.0 / 60, input); };
    menu.render(device, Mat4{1}, 640, 448);
    CHECK_FALSE(device.draws.empty());
    SECTION("resume and quit confirmation") {
        CHECK(step(back) == PauseOutcome::Resume);
        for (s32 i = 0; i < 3; ++i) {
            step(down);
        }
        step(select);
        CHECK(step(select) == PauseOutcome::Running); // default No
        for (s32 i = 0; i < 3; ++i) {
            step(down);
        }
        step(select);
        step(down);
        CHECK(step(select) == PauseOutcome::Title);
    }
    SECTION("shared settings") {
        step(select);
        step(select); // settings -> audio
        MenuInput left;
        left.left = true;
        step(left);
        step({});
        REQUIRE(settingsSaved);
        CHECK(AudioSlider::value(config.audio.musicVolume) == 127);
        step(back);
        step(back);
        CHECK(step(back) == PauseOutcome::Resume);
    }
    SECTION("save and reload") {
        step(down);
        step(select);
        step(select); // manage -> save
        step(select); // save -> slot 1
        REQUIRE(menu.party()[0].slot == 0);
        CHECK_FALSE(party[0].slot.has_value());
        CHECK(step(select) == PauseOutcome::Running); // acknowledge saved
        step(down);
        step(select);
        step(select); // load -> slot -> confirm
        CHECK(menu.menu().definition().title == strings.get("files.loadConfirm"));
        step(down);
        step(select);
        CHECK(menu.party()[0].save.gold == 123);
        CHECK(step(back) == PauseOutcome::Reload);
    }
    menu.close();
    CHECK_FALSE(menu.isOpen());
}

TEST_CASE("level abort uses the retail parchment dialog without character-file warnings",
          "[pause][unpacked]") {
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    GameContext context;
    context.tower = &world;
    context.unpackedRoot = root;
    context.strings = &strings;
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 0));
    MenuInput down;
    down.down = true;
    MenuInput select;
    select.select = true;
    menu.update(1.0 / 60, down);
    menu.update(1.0 / 60, select);
    const auto& definition = menu.menu().definition();
    CHECK(definition.title == "Abort Level?");
    CHECK(definition.items[0].text == "No");
    CHECK(definition.items[1].text == "Yes");
    CHECK(definition.body.empty());
    CHECK(definition.playerLabel.empty());
    CHECK_FALSE(definition.prompts);
    CHECK(definition.parchmentFont);
    CHECK(definition.colors.off.r == 92);
    CHECK(definition.colors.off.g == 26);
    CHECK(definition.colors.off.b == 3);
    CHECK(definition.backdropY == 64);
    CHECK(definition.backdropWidth == 320);
    CHECK(definition.backdropHeight == 220);
    menu.update(1, {});
    menu.update(1.0 / 60, down);
    const auto outcome = menu.update(1.0 / 60, select);
    CHECK(
        (outcome == PauseOutcome::ReturnTower || menu.update(1, {}) == PauseOutcome::ReturnTower));
}
} // namespace
