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
TEST_CASE("pause menus route character management and preserve the live party", "[pause][assets]") {
    test::FakeRenderDevice device;
    GameConfig config;
    config.save.directory = test::scratchDirectory("pause-files").string();
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
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
    CHECK(menu.menu().definition().parchmentFont);
    CHECK_FALSE(menu.menu().definition().prompts);
    CHECK(menu.menu().definition().backLabel.empty());
    CHECK(menu.menu().definition().selectLabel.empty());
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
        for (s32 i = 0; i < 4; ++i) {
            step(down);
        }
        step(select);
        // The retail dialog (0x8011EB1C) is the abort dialog's parchment with its own
        // title: No first, Yes, and nothing else on it.
        const auto& dialog = menu.menu().definition();
        CHECK(dialog.title == "Quit Game?");
        REQUIRE(dialog.items.size() == 2);
        CHECK(dialog.items[0].text == "No");
        CHECK(dialog.items[1].text == "Yes");
        CHECK(dialog.body.empty());
        CHECK(dialog.playerLabel.empty());
        CHECK_FALSE(dialog.prompts);
        CHECK(dialog.parchmentFont);
        CHECK(dialog.fades);
        CHECK(dialog.x == -256);
        CHECK(dialog.backdropY == 64);
        CHECK(dialog.backdropWidth == 320);
        CHECK(dialog.backdropHeight == 220);
        menu.update(1, {});
        CHECK(step(select) == PauseOutcome::Running); // default No
        for (s32 i = 0; i < 4; ++i) {
            step(down);
        }
        step(select);
        menu.update(1, {});
        step(down);
        CHECK(step(select) == PauseOutcome::Title);
    }
    SECTION("shop and inventory") {
        // OPTMENU_TOWER: OPT_SHOP opens init_shop(1), OPT_INVENTORY init_shop(2).
        step(down);
        step(down);
        CHECK(step(select) == PauseOutcome::Shop);
        step(down);
        CHECK(step(select) == PauseOutcome::Inventory);
    }
    SECTION("music under the menu") {
        // options.c 813 ducks the music every frame the menu is up; 984 lets it play on
        // the Audio page, where its slider is set.
        CHECK_FALSE(menu.musicAudible());
        step(select);
        CHECK_FALSE(menu.musicAudible());
        step(select); // settings -> audio
        CHECK(menu.musicAudible());
        step(back);
        CHECK_FALSE(menu.musicAudible());
        step(back);
        CHECK(step(back) == PauseOutcome::Resume);
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
    SECTION("character management returns to party selection") {
        step(down);
        CHECK(step(select) == PauseOutcome::Manage);
        REQUIRE_FALSE(menu.party()[0].slot.has_value());
        CHECK_FALSE(party[0].slot.has_value());
        CHECK(menu.party()[0].save.gold == 123);
    }
    menu.close();
    CHECK_FALSE(menu.isOpen());
}

TEST_CASE("level abort uses the retail parchment dialog without character-file warnings",
          "[pause][assets]") {
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
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

TEST_CASE("the secret world cannot be quit from its menu", "[pause][assets]") {
    // options.c 1462: OPT_QUITLEVEL's value is -1 (greyed) while sMusicTrackHi is 12.
    test::FakeRenderDevice device;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    // A level of the secret realm that is not unpacked: the world keeps the reference.
    LevelRef secret;
    secret.realm = "SECRET";
    secret.realmId = LevelRef::kSecretRealm;
    secret.name = "S1";
    secret.directory = "LEVELS/LEVELS1-absent";
    LevelWorld world;
    REQUIRE_FALSE(world.load(device, root, secret));
    REQUIRE(world.ref().isSecret());
    GameContext context;
    context.tower = &world;
    context.unpackedRoot = root;
    context.strings = &strings;
    const std::array party{PartyMember{}};
    PauseMenu menu;
    REQUIRE(menu.open(device, context, party, 0));
    const auto& items = menu.menu().definition().items;
    REQUIRE(items.size() == 2);
    CHECK(items[0].text == strings.get("pause.settings"));
    CHECK(items[0].enabled);
    CHECK(items[1].text == strings.get("pause.quitLevel"));
    CHECK_FALSE(items[1].enabled);
    MenuInput down;
    down.down = true;
    MenuInput select;
    select.select = true;
    menu.update(1.0 / 60, down);
    CHECK(menu.update(1.0 / 60, select) == PauseOutcome::Running);
    CHECK(menu.menu().definition().items.size() == 2); // still the main page
}
} // namespace
