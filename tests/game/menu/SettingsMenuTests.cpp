#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/platform/Input.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/menu/SettingsMenu.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Fixture {
    BitmapFont font = BitmapFont::fromGlyphs(32, 16, {});
    test::FakeTexture texture{64, 64};
    TextPainter painter;
    StringTable strings;
    GameConfig config;
    SettingsMenu menu;
    bool fail = false;
    s32 writes = 0;
    Fixture() {
        painter.setFont(&font, &texture);
        strings.load(test::dataDirectory() / "text", "en");
        menu.open(config, &strings,
                  [this](const GameConfig& next) {
                      if (fail) {
                          return false;
                      }
                      config = next;
                      ++writes;
                      return true;
                  },
                  painter, {}, {});
    }
    void down() {
        MenuInput input;
        input.down = true;
        menu.update(input, 1);
    }
    void select() {
        MenuInput input;
        input.select = true;
        menu.update(input, 1);
    }
    void back() {
        MenuInput input;
        input.back = true;
        menu.update(input, 1);
    }
    void right() {
        MenuInput input;
        input.right = true;
        menu.update(input, 1);
    }
};

TEST_CASE("settings audio changes persist transactionally and stay bounded", "[settings]") {
    Fixture f;
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Audio);
    MenuInput left;
    left.left = true;
    f.menu.update(left, 1);
    CHECK(std::abs(f.config.audio.masterVolume - 0.95f) < 0.0001f);
    CHECK(f.writes == 1);
    f.fail = true;
    f.menu.update(left, 1);
    CHECK(f.writes == 1);
    CHECK(f.menu.config().audio.masterVolume == f.config.audio.masterVolume);
    f.fail = false;
    for (s32 i = 0; i < 30; ++i) {
        f.menu.update(left, 1);
    }
    CHECK(f.config.audio.masterVolume == 0);
    f.down();
    f.right();
    CHECK(std::abs(f.config.audio.musicVolume - 0.75f) < 0.0001f);
    f.down();
    f.menu.update(left, 1);
    CHECK(std::abs(f.config.audio.effectsVolume - 0.95f) < 0.0001f);
}

TEST_CASE("difficulty and compass options survive a configuration save", "[settings]") {
    Fixture f;
    f.down();
    f.select();
    f.right();
    REQUIRE(f.config.difficulty.level == "hard");
    f.back();
    f.down();
    f.select();
    f.right();
    REQUIRE_FALSE(f.config.camera.compass);
    const auto file = test::scratchDirectory("menu-settings") / "settings.json";
    f.config.saveFile(file);
    GameConfig restored;
    REQUIRE(restored.loadFile(file));
    CHECK(restored.toJson() == f.config.toJson());
    f.back();
    MenuInput back;
    back.back = true;
    CHECK(f.menu.update(back, 1).action == MenuAction::Back);
}

TEST_CASE("controls capture keyboard and any title controller without consuming confirmation",
          "[settings]") {
    Fixture f;
    f.down();
    f.down();
    f.down();
    f.select();
    f.down();
    f.down();
    f.select();
    REQUIRE(f.menu.capturing());
    Input input;
    input.setKey(Key::P, true);
    f.menu.update({}, 1, &input);
    CHECK_FALSE(f.menu.capturing());
    CHECK(f.config.play.up == std::vector<Key>{Key::P});
    CHECK(f.config.play.actionChords);
    CHECK(f.config.play.padMagicGestures);
    f.select();
    input.beginPoll();
    input.setKey(Key::Escape, true);
    f.menu.update({}, 1, &input);
    CHECK_FALSE(f.menu.capturing());
    CHECK(f.writes == 1);
    // Re-enter Controls, toggle device, choose binding; controller 3 can capture.
    f.back();
    f.select();
    f.select();
    f.down();
    f.down();
    f.select();
    input.beginPoll();
    input.setKey(Key::Escape, false);
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::RightBumper)] = true;
    input.setPad(2, pad);
    f.menu.update({}, 1, &input);
    CHECK(f.config.play.padUp == std::vector<PadButton>{PadButton::RightBumper});
    f.down();
    f.select();
    CHECK(f.config.play.padUp.empty());
    f.down();
    f.select();
    CHECK(f.config.play.padUp == PlayBindings{}.padUp);
    CHECK(f.config.play.up == PlayBindings{}.up);
}

TEST_CASE("controls reject pause bindings and only capture the owning pad in play", "[settings]") {
    Fixture f;
    f.down();
    f.down();
    f.down();
    f.select();
    f.select();
    f.down();
    f.down();
    f.select();
    Input input;
    PadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<usize>(PadButton::Start)] = true;
    input.setPad(1, pad);
    f.menu.update({}, 1, &input, 1);
    REQUIRE(f.menu.capturing());
    pad.buttons[static_cast<usize>(PadButton::A)] = true;
    input.setPad(0, pad);
    f.menu.update({}, 1, &input, 1);
    REQUIRE(f.menu.capturing());
    input.beginPoll();
    input.setPad(1, pad);
    f.menu.update({}, 1, &input, 1);
    REQUIRE_FALSE(f.menu.capturing());
    CHECK(f.config.play.padUp == std::vector<PadButton>{PadButton::A});
}
} // namespace
