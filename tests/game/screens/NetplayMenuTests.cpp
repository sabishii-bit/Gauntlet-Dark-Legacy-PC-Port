#include <catch2/catch_test_macros.hpp>

#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "game/screens/NetplayMenu.h"

using namespace gdl;
using namespace gdl::game;
TEST_CASE("netplay menu drills into host join and readiness without starting from a settings click",
          "[netplay][netplay-menu]") {
    test::FakeRenderDevice device;
    NetplayMenu menu;
    NetplayMenu::View view;
    MenuInput confirm;
    confirm.select = true;
    MenuInput down;
    down.down = true;
    MenuInput left;
    left.left = true;
    menu.show(device, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Host);
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Join);
    view.page = NetplayMenu::Page::Host;
    view.invitation = std::string(300, 'a');
    view.connected = true;
    view.players = 2;
    menu.show(device, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Copy);
    menu.update(down);
    const auto setting = menu.update(left);
    CHECK(setting.action == NetplayMenu::Action::Capacity);
    CHECK(setting.direction == -1);
    menu.update(down);
    menu.update(down);
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::SelectCharacters);
    menu.update(down);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Leave);
    CHECK(menu.view().invitation == view.invitation);
    menu.render(device, makeScreenProjection(1280, 720), 1280, 720);
    CHECK_FALSE(device.draws.empty());
}
TEST_CASE("online menu mouse hitboxes honor letterboxing and disabled actions",
          "[netplay][netplay-menu]") {
    test::FakeRenderDevice device;
    NetplayMenu menu;
    NetplayMenu::View view;
    MenuInput confirm;
    confirm.select = true;
    MenuInput escape;
    escape.escape = true;
    view.page = NetplayMenu::Page::Join;
    menu.show(device, view);
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
    menu.show(device, view);
    CHECK(menu.update(click).action == NetplayMenu::Action::Connect);
    view.page = NetplayMenu::Page::Pause;
    menu.show(device, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Resume);
    CHECK(menu.update(escape).action == NetplayMenu::Action::Back);
    view.status = std::string(300, '\n');
    CHECK_NOTHROW(menu.show(device, view));
    view.page = NetplayMenu::Page::Choose;
    view.available = false;
    menu.show(device, view);
    CHECK(menu.update(confirm).action == NetplayMenu::Action::Back);
}
