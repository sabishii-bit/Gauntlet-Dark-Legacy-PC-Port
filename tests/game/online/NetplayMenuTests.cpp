#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/online/NetplayMenu.h"

using namespace gdl;
using namespace gdl::game;
namespace {
/** What the shown page's rows read, top to bottom. */
std::vector<std::string> labels(const NetplayMenu& menu) {
    std::vector<std::string> shown;
    for (usize row = 0; row < menu.rows(); ++row) {
        shown.push_back(menu.label(row));
    }
    return shown;
}
} // namespace
TEST_CASE("netplay menu drills into host join and readiness without starting from a settings click",
          "[netplay][netplay-menu]") {
    test::FakeRenderDevice device;
    const StringTable strings;
    NetplayMenu menu;
    NetplayMenu::View view;
    MenuInput confirm;
    confirm.select = true;
    MenuInput down;
    down.down = true;
    MenuInput left;
    left.left = true;
    menu.show(device, strings, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Host);
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Join);
    view.page = NetplayMenu::Page::Host;
    view.invitation = std::string(300, 'a');
    view.connected = true;
    view.players = 2;
    menu.show(device, strings, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Copy);
    menu.update(down);
    const auto setting = menu.update(left);
    CHECK(setting.action == NetplayMenu::Action::Capacity);
    CHECK(setting.direction == -1);
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::GameSettings);
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::SelectCharacters);
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Leave);
    CHECK(menu.view().invitation == view.invitation);
    menu.render(device, makeScreenProjection(1280, 720), 1280, 720);
    CHECK_FALSE(device.draws.empty());
}
TEST_CASE("the game settings page lets the host set the rules and a guest read them",
          "[netplay][netplay-menu][room-settings]") {
    test::FakeRenderDevice device;
    const StringTable strings;
    NetplayMenu menu;
    NetplayMenu::View view;
    view.page = NetplayMenu::Page::Settings;
    view.host = true;
    view.connected = true;
    view.rules = {"Rage Mode", "Calm Mode"};
    view.settings.mechanics = 0b10;
    menu.show(device, strings, view);
    REQUIRE(menu.rows() == 6);
    MenuInput confirm;
    confirm.select = true;
    MenuInput down;
    down.down = true;
    MenuInput right;
    right.right = true;
    const auto difficulty = menu.update(right);
    CHECK(difficulty.action == NetplayMenu::Action::Difficulty);
    CHECK(difficulty.direction == 1);
    menu.update(down);
    CHECK(menu.update(right).action == NetplayMenu::Action::FriendlyFire);
    menu.update(down);
    CHECK(menu.update(right).action == NetplayMenu::Action::Camera);
    // A row each for the mechanics the build offers a room, by bit.
    for (const u8 rule : {0, 1}) {
        menu.update(down);
        const auto event = menu.update(right);
        CHECK(event.action == NetplayMenu::Action::Rule);
        CHECK(event.direction == 1);
        CHECK(event.rule == rule);
    }
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Back);
    // A guest reads the rules: every row but Back is greyed, so the cursor rests on Back.
    view.host = false;
    view.page = NetplayMenu::Page::Settings;
    menu.show(device, strings, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Back);
    menu.update(down);
    CHECK(menu.update(right).action == NetplayMenu::Action::None);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Back);
    // The joined page reaches the same page, read only: its first live row.
    view.page = NetplayMenu::Page::Guest;
    menu.show(device, strings, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::GameSettings);
    menu.render(device, makeScreenProjection(1280, 720), 1280, 720);
    CHECK_FALSE(device.draws.empty());
}

TEST_CASE("online menu mouse hitboxes honor letterboxing and disabled actions",
          "[netplay][netplay-menu]") {
    test::FakeRenderDevice device;
    const StringTable strings;
    NetplayMenu menu;
    NetplayMenu::View view;
    MenuInput confirm;
    confirm.select = true;
    MenuInput escape;
    escape.escape = true;
    view.page = NetplayMenu::Page::Join;
    menu.show(device, strings, view);
    const auto projection = makeLetterboxProjection(640, 448, 1920, 1080);
    menu.render(device, projection, 640, 448);
    const auto transform = makeVirtualScreenTransform(projection, 512, 384, 640, 448);
    const auto area = NetplayMenu::rowArea(2);
    const auto clip = transform * Vec4{area.x + 10, area.y + 10, 0.5f, 1};
    MenuInput click;
    click.pointer = (Vec2{clip} + Vec2{1}) / 2.0f;
    click.pointerNormalized = true;
    click.pointerPressed = true;
    CHECK(menu.update(click).action == NetplayMenu::Action::None);
    view.invitation = "private-ticket";
    menu.show(device, strings, view);
    CHECK(menu.update(click).action == NetplayMenu::Action::Connect);
    view.page = NetplayMenu::Page::Pause;
    menu.show(device, strings, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Resume);
    CHECK(menu.update(escape).action == NetplayMenu::Action::Back);
    view.status = std::string(300, '\n');
    CHECK_NOTHROW(menu.show(device, strings, view));
    view.page = NetplayMenu::Page::Choose;
    view.available = false;
    menu.show(device, strings, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Back);
}

TEST_CASE("online completed progress and terminal return are explicit local actions",
          "[netplay][netplay-menu][native-save]") {
    test::FakeRenderDevice device;
    const StringTable strings;
    NetplayMenu menu;
    NetplayMenu::View view;
    view.page = NetplayMenu::Page::Finished;
    menu.show(device, strings, view);
    CHECK(menu.rows() == 2);
    MenuInput confirm;
    confirm.select = true;
    CHECK(menu.update(confirm).action == NetplayMenu::Action::ReturnTitle);
    view.canSaveCompletedProgress = true;
    menu.show(device, strings, view);
    MenuInput up;
    up.up = true;
    menu.update(up);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::SaveCompletedProgress);
    MenuInput down;
    down.down = true;
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::ReturnTitle);
    view.page = NetplayMenu::Page::Pause;
    menu.show(device, strings, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Resume);
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::SaveCompletedProgress);
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Leave);
}

TEST_CASE("the online pages read the shipped text table, naming difficulties as the original",
          "[netplay][netplay-menu][text]") {
    StringTable strings;
    if (!strings.load(test::dataDirectory() / "text", "en")) {
        SKIP("data/text/en.json is not available");
    }
    test::FakeRenderDevice device;
    NetplayMenu menu;
    NetplayMenu::View view;
    view.localPlayers = 2;
    menu.show(device, strings, view);
    CHECK(menu.title() == "Netplay");
    CHECK(labels(menu) ==
          std::vector<std::string>{"Host Game", "Join Game", "Local players    < 2 >", "Back"});
    view.page = NetplayMenu::Page::Host;
    view.players = 3;
    menu.show(device, strings, view);
    CHECK(menu.title() == "Host Game");
    CHECK(labels(menu) == std::vector<std::string>{"Copy invitation code",
                                                   "Player limit    < 4 >    (3 joined)",
                                                   "Game Settings", "Select 2 local character(s)",
                                                   "Ready", "Start Game", "Leave Room"});
    view.page = NetplayMenu::Page::Guest;
    view.ready = true;
    menu.show(device, strings, view);
    CHECK(menu.title() == "Joined Game");
    CHECK(labels(menu) == std::vector<std::string>{"Connected to host",
                                                   "Player limit    < 4 >    (3 joined)",
                                                   "Game Settings", "Select 2 local character(s)",
                                                   "Not Ready", "Leave Room"});
    // The difficulties are the Difficulty page's, the original's words (GC OPTMENU 0x8011EC94:
    // Easy 0x80347550, Medium 0x80347558, Hard 0x80347560); a room rule is a switch's Off or On.
    view.page = NetplayMenu::Page::Settings;
    view.rules = {"Rage Mode", "Calm Mode"};
    view.settings.mechanics = 0b01;
    menu.show(device, strings, view);
    CHECK(menu.title() == "Game Settings");
    CHECK(labels(menu) == std::vector<std::string>{"Difficulty    < Medium >",
                                                   "Friendly fire    < Off >",
                                                   "Camera    < Shared >", "Rage Mode    < On >",
                                                   "Calm Mode    < Off >", "Back"});
    view.settings.difficulty = 0;
    view.settings.friendlyFire = 1;
    view.settings.camera = 1;
    menu.show(device, strings, view);
    CHECK(menu.label(0) == "Difficulty    < Easy >");
    CHECK(menu.label(1) == "Friendly fire    < Stun >");
    CHECK(menu.label(2) == "Camera    < Solo >");
    view.settings.difficulty = 2;
    view.settings.friendlyFire = 2;
    menu.show(device, strings, view);
    CHECK(menu.label(0) == "Difficulty    < Hard >");
    CHECK(menu.label(1) == "Friendly fire    < Damage >");
    view.page = NetplayMenu::Page::Join;
    menu.show(device, strings, view);
    CHECK(menu.title() == "Join Game");
    CHECK(labels(menu) ==
          std::vector<std::string>{"Paste invitation code", "Clear code", "Connect", "Back"});
    view.page = NetplayMenu::Page::Pause;
    menu.show(device, strings, view);
    CHECK(menu.title() == "Online Menu - game continues");
    CHECK(labels(menu) ==
          std::vector<std::string>{"Resume", "Save Completed Progress", "Leave Game"});
    view.page = NetplayMenu::Page::Finished;
    menu.show(device, strings, view);
    CHECK(menu.title() == "Online Session Ended");
    CHECK(labels(menu) == std::vector<std::string>{"Save Completed Progress", "Return to Title"});
    view.page = NetplayMenu::Page::Leave;
    menu.show(device, strings, view);
    CHECK(menu.title() == "Leave the online game?");
    CHECK(labels(menu) == std::vector<std::string>{"Cancel", "Leave Game"});
}

TEST_CASE("the online pages take a translation's words and show an entry that does not format",
          "[netplay][netplay-menu][text]") {
    const auto dir = test::scratchDirectory("netplay-menu-words");
    writeTextFile(dir / "en.json",
                  R"json({"netplay.host": "Heberger", "netplay.localPlayers": "Joueurs",
        "netplay.joinedCount": "({} ici)", "netplay.selectCharacters": "Choisir {x}",
        "settings.difficulty": "Niveau", "settings.normal": "Moyen", "settings.on": "Oui"})json");
    StringTable strings;
    REQUIRE(strings.load(dir, "en"));
    test::FakeRenderDevice device;
    NetplayMenu menu;
    NetplayMenu::View view;
    menu.show(device, strings, view);
    CHECK(menu.title() == "netplay.title"); // an id the table lacks reads as itself
    CHECK(menu.label(0) == "Heberger");
    CHECK(menu.label(2) == "Joueurs    < 1 >");
    view.page = NetplayMenu::Page::Host;
    view.players = 2;
    menu.show(device, strings, view);
    CHECK(menu.title() == "Heberger");
    CHECK(menu.label(1) == "netplay.playerLimit    < 4 >    (2 ici)");
    CHECK(menu.label(3) == "Choisir {x}");
    view.page = NetplayMenu::Page::Settings;
    view.rules = {"Rage"};
    view.settings.mechanics = 1;
    menu.show(device, strings, view);
    CHECK(menu.label(0) == "Niveau    < Moyen >");
    CHECK(menu.label(3) == "Rage    < Oui >");
}
