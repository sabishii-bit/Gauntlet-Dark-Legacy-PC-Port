#include <algorithm>

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
    void release() { menu.update({}, 1); }
};

TEST_CASE("settings audio changes persist transactionally and stay bounded", "[settings]") {
    Fixture f;
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Audio);
    MenuInput left;
    left.left = true;
    f.menu.update(left, 1);
    CHECK(f.writes == 0);
    CHECK(AudioSlider::value(f.menu.config().audio.musicVolume) == 127);
    f.release();
    CHECK(f.writes == 1);
    f.fail = true;
    f.menu.update(left, 1);
    f.release();
    CHECK(f.writes == 1);
    CHECK(AudioSlider::value(f.config.audio.musicVolume) == 127);
    CHECK(AudioSlider::value(f.menu.config().audio.musicVolume) == 126);
    f.fail = false;
    left.left = false;
    left.leftHeld = true;
    f.menu.update(left, 255);
    f.release();
    CHECK(f.config.audio.musicVolume == 0);
    f.down();
    f.right();
    f.release();
    CHECK(AudioSlider::value(f.config.audio.effectsVolume) == 129);
    f.down();
    f.menu.update(left, 1);
    CHECK(f.menu.config().audio.stereo); // held alone does not repeat the mode toggle
    CHECK(f.menu.menu().definition().items[2].markedPart == 2);
    f.right();
    CHECK(f.menu.menu().definition().items[2].markedPart == 1);
    f.release();
    CHECK_FALSE(f.config.audio.stereo);
    f.select();
    CHECK_FALSE(f.menu.config().audio.stereo); // confirm is a no-op
    left.leftHeld = false;
    left.left = true;
    f.menu.update(left, 1);
    CHECK(f.menu.menu().definition().items[2].markedPart == 2);
    f.release();
    CHECK(f.config.audio.stereo);
    CHECK(f.config.audio.masterVolume == 1);
}

TEST_CASE("options omit generic instructions but retain actionable notices", "[settings]") {
    Fixture f;
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = 'A'; c <= 'Z'; ++c) {
        glyphs.push_back({c, 16, 0, 0});
    }
    auto font = BitmapFont::fromGlyphs(32, 16, std::move(glyphs));
    f.painter.setFont(&font, &f.texture);
    const auto footerDrawn = [&] {
        test::FakeRenderDevice device;
        Canvas canvas;
        canvas.begin(device, Mat4{1});
        f.menu.draw(canvas, f.painter, {});
        canvas.end();
        return std::ranges::any_of(device.draws, [](const auto& draw) {
            return std::ranges::any_of(draw.vertices,
                                       [](const auto& vertex) { return vertex.position.y >= 276; });
        });
    };
    CHECK_FALSE(footerDrawn()); // options root
    f.select();
    f.fail = true;
    f.right();
    f.release();
    CHECK(footerDrawn()); // failed persistence remains visible
    f.fail = false;
    f.back();
    f.down();
    f.select();
    f.select();
    CHECK(footerDrawn()); // difficulty applies next level
    f.back();
    f.back();
    f.down();
    f.select();
    CHECK_FALSE(footerDrawn()); // compass
    f.back();
    f.down();
    f.select();
    CHECK_FALSE(footerDrawn()); // controls
    f.down();
    f.down();
    f.select();
    CHECK(f.menu.capturing());
    CHECK(footerDrawn()); // binding capture still explains what it is waiting for
}

TEST_CASE("difficulty and compass options survive a configuration save", "[settings]") {
    Fixture f;
    f.down();
    f.select();
    f.select(); // difficulty submenu
    f.down();
    f.select();
    REQUIRE(f.config.difficulty.level == "hard");
    f.back();
    f.back();
    f.down();
    f.select();
    f.down();
    f.select();
    REQUIRE(f.config.camera.compass);
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

TEST_CASE("retail menu scopes exclude title-only and tower-only settings", "[settings]") {
    Fixture f;
    const auto open = [&](SettingsMenu::Scope scope) {
        f.menu.open(f.config, &f.strings, {}, f.painter, {}, {}, scope);
        std::vector<s32> codes;
        for (const auto& item : f.menu.menu().definition().items) {
            codes.push_back(item.code);
        }
        return codes;
    };
    CHECK(open(SettingsMenu::Scope::Title) == std::vector<s32>{0, 1, 2, 3});
    CHECK(open(SettingsMenu::Scope::Tower) == std::vector<s32>{0, 2, 3});
    CHECK(open(SettingsMenu::Scope::Level) == std::vector<s32>{0, 3});
}

TEST_CASE("audio previews held ticks and persists once on release", "[settings]") {
    Fixture f;
    s32 previews = 0;
    f.menu.open(
        f.config, &f.strings,
        [&](const auto&) {
            ++f.writes;
            return true;
        },
        f.painter, {}, {}, SettingsMenu::Scope::Title,
        [&](const AudioConfig& audio) {
            ++previews;
            CHECK(audio.masterVolume == 1);
        });
    f.select();
    REQUIRE(f.menu.menu().definition().items.size() == 3);
    CHECK(f.menu.menu().definition().items[0].text == "Music Volume");
    CHECK(f.menu.menu().definition().items[0].extraSpacing == 52);
    CHECK(f.menu.menu().definition().x == 128);
    CHECK(f.menu.menu().itemY(0) == 108);
    f.select();
    CHECK(previews == 0);
    MenuInput hold;
    hold.rightHeld = true;
    f.menu.update(hold, 2);
    f.menu.update(hold, 6);
    CHECK(AudioSlider::value(f.menu.config().audio.musicVolume) == 136);
    CHECK(previews == 2);
    CHECK(f.writes == 0);
    f.back();
    CHECK(f.writes == 1);
    CHECK(f.menu.page() == SettingsMenu::Page::Root);
}

TEST_CASE("volume sliders use the five original sprite extents and inactive opacities",
          "[settings]") {
    test::FakeTexture left{64, 64};
    test::FakeTexture empty{128, 32};
    test::FakeTexture fill{128, 32};
    test::FakeTexture knob{32, 64};
    test::FakeTexture right{64, 64};
    const AudioSlider slider{{&left, &empty, &fill, &knob, &right}};
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    slider.draw(canvas, 128, 133, 0, false, 255);
    canvas.end();
    REQUIRE(device.draws.size() == 5);
    CHECK(Vec2(device.draws[0].vertices[0].position) == Vec2{76, 133});
    CHECK(Vec2(device.draws[1].vertices[0].position) == Vec2{128, 144});
    const auto& fillDraw = device.draws[2];
    const auto [min, max] = std::ranges::minmax_element(fillDraw.vertices, {},
                                                        [](const auto& v) { return v.position.x; });
    CHECK(max->position.x - min->position.x == 1);
    CHECK(fillDraw.vertices[0].color == Color::white().withAlpha(95));
    CHECK(Vec2(device.draws[3].vertices[0].position) == Vec2{109, 135});
    CHECK(Vec2(device.draws[4].vertices[0].position) == Vec2{368, 133});
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
