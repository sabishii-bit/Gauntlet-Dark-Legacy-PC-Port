#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/ui/TextPainter.h"

#include "FakeGameServices.h"
#include "FakeMechanic.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/ControlProfiles.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/menu/SettingsMenu.h"
#include "game/menu/SettingsRows.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Fixture {
    BitmapFont font = BitmapFont::fromGlyphs(32, 16, {});
    test::FakeTexture texture{64, 64};
    TextPainter painter;
    StringTable strings;
    GameConfig config;
    test::FakeSettings settings; ///< writes its saves back into `config`
    SettingsMenu menu;
    explicit Fixture(const MechanicCatalog* mechanics = nullptr) {
        painter.setFont(&font, &texture);
        strings.load(test::dataDirectory() / "text", "en");
        settings.target = &config;
        menu.setMechanics(mechanics);
        menu.open(config, &strings, &settings, painter, {}, {});
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
    void focus(s32 code) {
        const auto& items = menu.menu().definition().items;
        const auto item = std::ranges::find(items, code, &MenuItem::code);
        REQUIRE(item != items.end());
        menu.menu().focus(static_cast<usize>(item - items.begin()));
    }
    void choose(s32 code) {
        focus(code);
        select();
    }
    void applyControls() {
        const auto& items = menu.menu().definition().items;
        const auto apply = std::ranges::find(items, 104, &MenuItem::code);
        REQUIRE(apply != items.end());
        menu.menu().focus(static_cast<usize>(apply - items.begin()));
        select();
    }
};

TEST_CASE("Game Options exposes optional shop stock and autosaving", "[settings][autosave][shop]") {
    Fixture f;
    f.choose(1);
    REQUIRE(f.menu.page() == SettingsMenu::Page::Game);
    REQUIRE(f.menu.menu().definition().items.size() == 6);
    CHECK(f.menu.menu().definition().items.front().text == "Difficulty");
    CHECK(f.menu.menu().definition().items.back().text == "Language");
    CHECK(f.menu.menu().definition().items.back().value == "English");
    f.choose(3);
    CHECK(f.config.game.expandedShop);
    CHECK_FALSE(f.config.game.autoSave);
    f.release();
    f.focus(4);
    f.right();
    CHECK(f.config.game.autoSave);
    f.release();
    f.settings.fail = true;
    f.right();
    CHECK(f.config.game.autoSave);
    CHECK(f.settings.writes == 2);
}

TEST_CASE("Game Options switches each mechanic the build carries, after its own switches",
          "[settings][mechanics]") {
    MechanicCatalog mechanics;
    REQUIRE(mechanics.add(test::FakeMechanic::descriptor("rage")));
    REQUIRE(mechanics.add(test::FakeMechanic::descriptor("calm", false)));
    Fixture f(&mechanics);
    f.choose(1);
    REQUIRE(f.menu.page() == SettingsMenu::Page::Game);
    const auto& items = f.menu.menu().definition().items;
    REQUIRE(items.size() == 8);
    CHECK(items[5].text == "calm"); // by id, each named by its own text
    CHECK(items[6].text == "rage");
    CHECK(items.back().text == "Language");
    CHECK_FALSE(MechanicCatalog::enabled(f.config.game, "rage"));
    f.focus(items[6].code);
    f.right();
    CHECK(MechanicCatalog::enabled(f.config.game, "rage"));
    CHECK_FALSE(MechanicCatalog::enabled(f.config.game, "calm"));
    f.release();
    f.focus(f.menu.menu().definition().items[6].code);
    f.right();
    CHECK_FALSE(MechanicCatalog::enabled(f.config.game, "rage"));
    CHECK(f.settings.writes == 2);
}

TEST_CASE("mouse operates settings pages sliders and graphics without keyboard confirmation",
          "[settings][mouse]") {
    Fixture f;
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = ' '; c <= '~'; ++c) {
        glyphs.push_back({c, 8, 0, 0});
    }
    f.font = BitmapFont::fromGlyphs(32, 8, std::move(glyphs));
    f.settings.video = [](const GameConfig&) { return true; };
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, MenuDefinition::parchment(),
                SettingsMenu::Scope::Title);
    const auto click = [&](usize index) {
        const auto area = f.menu.menu().itemArea(index);
        MenuInput input;
        input.pointer = Vec2{area.x + area.width / 2, area.y + area.height / 2};
        input.pointerPressed = true;
        f.menu.update(input, 1);
    };
    click(0);
    REQUIRE(f.menu.page() == SettingsMenu::Page::Audio);
    CHECK(f.settings.writes == 0);
    const auto track = AudioSlider::track(
        160, static_cast<f32>(f.menu.menu().itemY(0) + f.menu.menu().lineHeight()), 0.7f);
    MenuInput drag;
    drag.pointer = Vec2{track.x + track.width * 0.25f, track.y + 1};
    drag.pointerPressed = true;
    drag.pointerHeld = true;
    f.menu.update(drag, 1);
    CHECK(AudioSlider::value(f.menu.config().audio.musicVolume) == 64);
    CHECK(f.settings.writes == 0);
    drag.pointerPressed = false;
    drag.pointer->x = track.x + track.width;
    f.menu.update(drag, 1);
    CHECK(AudioSlider::value(f.menu.config().audio.musicVolume) == 255);
    drag.pointerHeld = false;
    f.menu.update(drag, 1);
    CHECK(f.settings.writes == 1);
    const auto stereo = f.menu.menu().itemArea(3);
    MenuInput mono;
    mono.pointer = Vec2{stereo.x + 1, stereo.y + 1};
    mono.pointerPressed = true;
    f.menu.update(mono, 1);
    CHECK_FALSE(f.config.audio.stereo);
    f.menu.update(mono, 1);
    CHECK_FALSE(f.config.audio.stereo);
    f.back();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Root);
    click(3);
    REQUIRE(f.menu.page() == SettingsMenu::Page::Graphics);
    const bool wasVsync = f.menu.config().display.vsync;
    click(1);
    CHECK(f.menu.config().display.vsync != wasVsync);
    const s32 writesBeforeVideo = f.settings.writes;
    click(12);
    REQUIRE(f.menu.menu().definition().items.size() == 2);
    CHECK(f.settings.writes == writesBeforeVideo);
    click(0);
    REQUIRE(f.menu.menu().definition().items.size() == 14);
    CHECK(f.settings.writes == writesBeforeVideo + 1);
    CHECK(f.config.display.vsync != wasVsync);
    // No Back row: the back control (the mouse's second button maps to it) leaves the page.
    MenuInput leave;
    leave.back = true;
    f.menu.update(leave, 1);
    CHECK(f.menu.page() == SettingsMenu::Page::Root);
}

TEST_CASE("settings audio changes persist transactionally and stay bounded", "[settings]") {
    Fixture f;
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Audio);
    MenuInput left;
    left.left = true;
    f.menu.update(left, 1);
    CHECK(f.settings.writes == 0);
    CHECK(AudioSlider::value(f.menu.config().audio.musicVolume) == 127);
    f.release();
    CHECK(f.settings.writes == 1);
    f.settings.fail = true;
    f.menu.update(left, 1);
    f.release();
    CHECK(f.settings.writes == 1);
    CHECK(AudioSlider::value(f.config.audio.musicVolume) == 127);
    CHECK(AudioSlider::value(f.menu.config().audio.musicVolume) == 126);
    f.settings.fail = false;
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
    f.down();
    f.menu.update(left, 1);
    CHECK(f.menu.config().audio.stereo); // held alone does not repeat the mode toggle
    CHECK(f.menu.menu().definition().items[3].markedPart == 2);
    f.right();
    CHECK(f.menu.menu().definition().items[3].markedPart == 1);
    f.release();
    CHECK_FALSE(f.config.audio.stereo);
    f.select();
    CHECK_FALSE(f.menu.config().audio.stereo); // confirm is a no-op
    left.leftHeld = false;
    left.left = true;
    f.menu.update(left, 1);
    CHECK(f.menu.menu().definition().items[3].markedPart == 2);
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
    f.settings.fail = true;
    f.right();
    f.release();
    CHECK(footerDrawn()); // failed persistence remains visible
    f.settings.fail = false;
    f.back();
    f.down();
    f.select();
    f.select();
    CHECK_FALSE(footerDrawn()); // no unsolicited difficulty footer
    f.back();
    f.back();
    f.down();
    f.select();
    CHECK_FALSE(footerDrawn()); // compass
}

TEST_CASE("multiplayer labels and both cursor representations fit the parchment",
          "[settings][menu]") {
    Fixture f;
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = 33; c <= 126; ++c) {
        glyphs.push_back({c, 24, 0, 0});
    }
    const auto font = BitmapFont::fromGlyphs(32, 12, std::move(glyphs));
    f.painter.setFont(&font, &f.texture);
    f.menu.open(f.config, &f.strings, nullptr, f.painter, {}, MenuDefinition::parchment());
    f.down();
    f.select();
    f.down();
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Multiplayer);
    const auto& definition = f.menu.menu().definition();
    CHECK(definition.scale <= 0.65f);
    CHECK(definition.cursorScale == definition.scale);
    CHECK(f.menu.menu().iconScale() == OptionMenu::iconPixelsPerUnit({}) * definition.scale);
    for (const auto& item : definition.items) {
        CHECK(definition.x + f.painter.measure(item.text + " ~", definition.scale) <= 512 - 64);
    }
    test::FakeRenderDevice device;
    test::FakeTexture arrows{32, 32};
    MenuTextures textures;
    textures.arrows = &arrows;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();
    const auto arrow = std::ranges::find_if(
        device.draws, [&](const auto& draw) { return draw.texture == &arrows; });
    REQUIRE(arrow != device.draws.end());
    const Vec2 size = test::maxCorner(*arrow) - test::minCorner(*arrow);
    CHECK(std::abs(size.x - 22 * definition.scale) < 0.001f);
    CHECK(std::abs(size.y - 20 * definition.scale) < 0.001f);
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

TEST_CASE("Game Options are available in title tower and level settings", "[settings]") {
    Fixture f;
    const auto open = [&](SettingsMenu::Scope scope) {
        f.menu.open(f.config, &f.strings, nullptr, f.painter, {}, {}, scope);
        std::vector<s32> codes;
        for (const auto& item : f.menu.menu().definition().items) {
            codes.push_back(item.code);
        }
        return codes;
    };
    CHECK(open(SettingsMenu::Scope::Title) == std::vector<s32>{0, 1, 2, 4, 3});
    CHECK(open(SettingsMenu::Scope::Tower) == std::vector<s32>{0, 1, 2, 4, 3});
    CHECK(open(SettingsMenu::Scope::Level) == std::vector<s32>{0, 1, 4, 3});
}

TEST_CASE("Combat auto melee changes are shared persisted and reversible from every scope",
          "[settings][combat-settings]") {
    for (const auto scope :
         {SettingsMenu::Scope::Title, SettingsMenu::Scope::Tower, SettingsMenu::Scope::Level}) {
        Fixture f;
        f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, MenuDefinition::parchment(),
                    scope);
        f.choose(1);
        REQUIRE(f.menu.page() == SettingsMenu::Page::Game);
        f.choose(2);
        REQUIRE(f.menu.page() == SettingsMenu::Page::Combat);
        CHECK(f.menu.menu().definition().title == "Combat");
        CHECK(f.menu.menu().definition().items[0].text == "Auto Melee");
        CHECK(f.menu.menu().definition().items[2].text == "Auto-Use Item");
        CHECK(f.menu.menu().definition().items[3].text == "Auto Aim");
        CHECK(f.menu.menu().definition().items[3].value == "On");
        CHECK(f.menu.menu().definition().items[0].value == "Off");
        f.settings.fail = true;
        f.right();
        CHECK_FALSE(f.config.combat.autoMelee);
        CHECK(f.menu.menu().definition().items[0].value == "Off");
        f.settings.fail = false;
        f.select();
        CHECK(f.config.combat.autoMelee);
        CHECK(f.menu.menu().definition().items[0].value == "On");
        CHECK(f.settings.writes == 1);
        f.back();
        CHECK(f.menu.page() == SettingsMenu::Page::Game);
        CHECK(f.menu.menu().selection() == 2);
        f.select();
        f.right();
        CHECK_FALSE(f.config.combat.autoMelee);
        CHECK(f.settings.writes == 2);
        f.choose(1);
        CHECK_FALSE(f.config.combat.enemyHealthBars);
        CHECK(f.menu.menu().definition().items[1].value == "Off");
        CHECK(f.config.combat.autoActivateItems);
        f.choose(2);
        CHECK_FALSE(f.config.combat.autoActivateItems);
        CHECK(f.menu.menu().definition().items[2].value == "Off");
        f.settings.fail = true;
        f.right();
        CHECK_FALSE(f.config.combat.autoActivateItems);
        f.settings.fail = false;
        f.right();
        CHECK(f.config.combat.autoActivateItems);
        f.choose(1);
        CHECK(f.config.combat.enemyHealthBars);
        f.choose(3);
        CHECK_FALSE(f.config.combat.autoAim);
        CHECK(f.menu.menu().definition().items[3].value == "Off");
        f.choose(3);
        CHECK(f.config.combat.autoAim);
        f.back();
        f.back();
        CHECK(f.menu.page() == SettingsMenu::Page::Root);
        CHECK(
            f.menu.menu().definition().items[static_cast<usize>(f.menu.menu().selection())].code ==
            1);
    }
}

TEST_CASE("movie volume supports keys and mouse dragging without touching other volumes",
          "[settings][movie-volume][mouse]") {
    Fixture f;
    const auto preview = [&] {
        REQUIRE_FALSE(f.settings.audioPreviews.empty());
        return f.settings.audioPreviews.back();
    };
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, MenuDefinition::parchment(),
                SettingsMenu::Scope::Level);
    f.select();
    f.menu.menu().focus(2);
    MenuInput left;
    left.leftHeld = true;
    f.menu.update(left, 255);
    CHECK(f.menu.config().audio.movieVolume == 0);
    CHECK(preview().movieVolume == 0);
    CHECK(f.config.audio.movieVolume == 1);
    f.settings.fail = true;
    f.back();
    CHECK(f.menu.page() == SettingsMenu::Page::Audio);
    CHECK(f.config.audio.movieVolume == 1);
    f.settings.fail = false;
    f.release();
    CHECK(f.config.audio.movieVolume == 0);
    const auto track = AudioSlider::track(
        160, static_cast<f32>(f.menu.menu().itemY(2) + f.menu.menu().lineHeight()), 0.7f);
    MenuInput drag;
    drag.pointer = Vec2{track.x + track.width / 2, track.y + 1};
    drag.pointerPressed = true;
    drag.pointerHeld = true;
    f.menu.update(drag, 1);
    CHECK(std::abs(preview().movieVolume - 0.5f) <= 1.0f / 255);
    CHECK(f.config.audio.movieVolume == 0);
    drag.pointerPressed = false;
    drag.pointer->x = track.x + track.width + 100;
    f.menu.update(drag, 1);
    CHECK(preview().movieVolume == 1);
    drag.pointerHeld = false;
    f.menu.update(drag, 1);
    CHECK(f.config.audio.movieVolume == 1);
    CHECK(f.config.audio.musicVolume == AudioConfig{}.musicVolume);
    CHECK(f.config.audio.effectsVolume == AudioConfig{}.effectsVolume);
    CHECK(f.config.audio.stereo);
    CHECK(f.menu.audioSample().empty());
    for (usize i = 0; i < 3; ++i) {
        const f32 sliderBottom =
            static_cast<f32>(f.menu.menu().itemY(i) + f.menu.menu().lineHeight()) + 64 * 0.7f;
        CHECK(sliderBottom < f.menu.menu().itemY(i + 1));
    }
    CHECK(f.menu.menu().itemY(3) + f.menu.menu().lineHeight() < 348);
}

TEST_CASE("Video stages discrete choices until Apply and ignores Confirm on setting rows",
          "[settings][graphics]") {
    // These labels and settings are the user-requested PC feature, not retail menu entries.
    Fixture f;
    f.down();
    f.down();
    f.down();
    REQUIRE(f.menu.menu().definition().items[3].text == "Video");
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Graphics);
    REQUIRE(f.menu.menu().definition().items.size() == 14);
    CHECK(f.menu.menu().definition().items[0].value == "Vulkan");
    CHECK(f.menu.menu().definition().items[1].value == "On");
    CHECK(f.menu.menu().definition().items[2].value == "30");
    CHECK(f.menu.menu().definition().items[3].value == "Off");
    f.down();
    f.settings.fail = true;
    f.select();
    CHECK(f.config.display.vsync);
    CHECK(f.menu.config().display.vsync);
    CHECK(f.settings.writes == 0);
    f.settings.fail = false;
    f.right();
    CHECK_FALSE(f.menu.config().display.vsync);
    CHECK(f.config.display.vsync);
    CHECK(f.settings.writes == 0);
    f.down();
    for (const u32 rate : {60U, 0U, 30U, 60U, 0U, 30U}) {
        f.right();
        CHECK(f.menu.config().display.maxFrameRate == rate);
        CHECK(f.menu.config().timing.gameplayFrameRate == rate);
        CHECK(f.menu.menu().definition().items[2].value ==
              (rate == 0 ? "Unlimited" : std::to_string(rate)));
    }
    f.settings.fail = true;
    f.select();
    CHECK(f.config.display.maxFrameRate == 30);
    CHECK(f.menu.config().timing.gameplayFrameRate == 30);
    f.settings.fail = false;
    MenuInput left;
    left.left = true;
    f.menu.update(left, 1);
    CHECK(f.menu.config().display.maxFrameRate == 0);
    f.down();
    for (const u32 samples : {2U, 4U, 1U}) {
        f.right();
        CHECK(f.menu.config().display.sampleCount == samples);
    }
    f.settings.fail = true;
    f.select();
    CHECK(f.config.display.sampleCount == 1);
    CHECK(f.menu.config().display.sampleCount == 1);
    f.back();
    CHECK(f.menu.page() == SettingsMenu::Page::Root);
    CHECK(f.menu.menu().selection() == 3);
    CHECK(f.menu.config().display.vsync);
    CHECK(f.settings.writes == 0);
}

TEST_CASE("Video chooses supported resolutions and borderless uses the desktop without losing the "
          "preference",
          "[settings][graphics]") {
    Fixture f;
    f.settings.displayOptions = {{1920, 1080}, {{1280, 720}, {1920, 1080}}};
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, SettingsMenu::Scope::Level);
    f.choose(4);
    REQUIRE(f.menu.menu().definition().title == "Video");
    f.down();
    f.down();
    f.down();
    REQUIRE(f.menu.menu().selection() == 5); // Windowed resolution is skipped.
    CHECK_FALSE(f.menu.menu().definition().items[4].enabled);
    f.right();
    CHECK(f.menu.config().display.windowMode == WindowMode::Fullscreen);
    MenuInput up;
    up.up = true;
    f.menu.update(up, 1);
    REQUIRE(f.menu.menu().selection() == 4);
    f.right();
    CHECK(f.menu.config().display.windowWidth == 1280);
    CHECK(f.menu.config().display.windowHeight == 720);
    f.down();
    f.right();
    CHECK(f.menu.config().display.windowMode == WindowMode::BorderlessFullscreen);
    CHECK(f.menu.menu().definition().items[4].value == "1920 x 1080");
    CHECK_FALSE(f.menu.menu().definition().items[4].enabled);
    CHECK(f.menu.config().display.windowWidth == 1280);
    f.right();
    CHECK(f.menu.config().display.windowMode == WindowMode::Windowed);
    CHECK(f.menu.menu().definition().items[4].value == "1280 x 720");
    CHECK_FALSE(f.menu.menu().definition().items[4].enabled);
}

TEST_CASE("fullscreen replaces a custom window size with an advertised desktop mode",
          "[settings][graphics]") {
    Fixture f;
    f.settings.displayOptions = {{1920, 1080}, {{1920, 1080}}};
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, SettingsMenu::Scope::Level);
    f.choose(4);
    f.down();
    f.down();
    f.down();
    REQUIRE(f.menu.menu().selection() == 5);
    f.right();
    CHECK(f.menu.config().display.windowMode == WindowMode::Fullscreen);
    CHECK(f.menu.config().display.windowWidth == 1920);
    CHECK(f.menu.config().display.windowHeight == 1080);
}

TEST_CASE("graphics page fits parchment and returns to its entry in every menu scope",
          "[settings][graphics]") {
    Fixture f;
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = 32; c <= 126; ++c) {
        glyphs.push_back({c, 24, 0, 0});
    }
    const auto font = BitmapFont::fromGlyphs(32, 12, std::move(glyphs));
    f.painter.setFont(&font, &f.texture);
    for (const auto scope :
         {SettingsMenu::Scope::Title, SettingsMenu::Scope::Tower, SettingsMenu::Scope::Level}) {
        f.menu.open(f.config, &f.strings, nullptr, f.painter, {}, MenuDefinition::parchment(),
                    scope);
        const auto& rows = f.menu.menu().definition().items;
        const auto graphics = std::ranges::find(rows, 4, &MenuItem::code);
        REQUIRE(graphics != rows.end());
        REQUIRE(graphics + 1 != rows.end());
        CHECK((graphics + 1)->code == 3);
        const auto selected = static_cast<s32>(graphics - rows.begin());
        for (s32 i = 0; i < selected; ++i) {
            f.down();
        }
        f.select();
        REQUIRE(f.menu.page() == SettingsMenu::Page::Graphics);
        const auto& definition = f.menu.menu().definition();
        CHECK(definition.cursorScale == definition.scale);
        CHECK(definition.colors.off == MenuDefinition::parchment().colors.off);
        CHECK(definition.colors.on == MenuDefinition::parchment().colors.on);
        for (usize i = 0; i < definition.items.size(); ++i) {
            const auto& item = definition.items[i];
            CHECK(f.menu.menu().itemX(static_cast<s32>(i)) +
                      f.painter.measure(item.text, definition.scale) <=
                  512 - 64);
            if (!item.value.empty()) {
                CHECK(f.menu.menu().itemY(static_cast<s32>(i)) >= f.menu.menu().titleBottom());
                CHECK(f.menu.menu().itemY(static_cast<s32>(i)) +
                          static_cast<s32>(32 * definition.scale) + 12 <
                      316);
                CHECK(definition.valueX > f.menu.menu().itemX(static_cast<s32>(i)) +
                                              f.painter.measure(item.text, definition.scale));
                CHECK(definition.valueX + f.painter.measure(item.value, definition.scale) + 11 <=
                      448);
            } else {
                CHECK(f.menu.menu().itemY(static_cast<s32>(i)) == 316);
            }
        }
        f.back();
        CHECK(f.menu.page() == SettingsMenu::Page::Root);
        CHECK(f.menu.menu().selection() == selected);
    }
}

TEST_CASE("the expanded Video page keeps native font rows and its action bar separate",
          "[settings][graphics][smooth-sprites][assets]") {
    Fixture f;
    const auto fontPath = test::assetOrSkip("FONTS/font32.fnt");
    REQUIRE(f.font.load(fontPath, 16));
    f.painter.setFont(&f.font, &f.texture);
    f.menu.open(f.config, &f.strings, nullptr, f.painter, {}, MenuDefinition::parchment());
    f.choose(4);
    const auto& definition = f.menu.menu().definition();
    REQUIRE(definition.items.size() == 14);
    REQUIRE(definition.items[0].text == "Renderer");
    REQUIRE(definition.items[11].text == "Smooth Sprites");
    const s32 height = f.painter.lineHeight(definition.scale);
    for (s32 row = 0; row < 11; ++row) {
        CAPTURE(row, height, definition.scale);
        CHECK(f.menu.menu().itemY(row) + height < f.menu.menu().itemY(row + 1));
    }
}

TEST_CASE("custom file frame rates are shown honestly until a supported choice is selected",
          "[settings][graphics]") {
    Fixture f;
    f.config.timing.gameplayFrameRate = 24;
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, SettingsMenu::Scope::Level);
    f.choose(4);
    CHECK(f.menu.menu().definition().items[2].value == "24");
    f.down();
    f.right();
    CHECK(f.menu.config().timing.gameplayFrameRate == 30);
    CHECK(f.menu.config().display.maxFrameRate == 30);
    CHECK(f.config.timing.gameplayFrameRate == 24);
}

TEST_CASE("audio previews held ticks and persists once on release", "[settings]") {
    Fixture f;
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, SettingsMenu::Scope::Title);
    f.select();
    REQUIRE(f.menu.menu().definition().items.size() == 4);
    CHECK(f.menu.menu().definition().items[0].text == "Music Volume");
    CHECK(f.menu.menu().definition().items[2].text == "Movie Volume");
    CHECK(f.menu.menu().definition().x == 160);
    CHECK(f.menu.menu().itemY(0) == 96);
    f.select();
    CHECK(f.settings.audioPreviews.empty());
    MenuInput hold;
    hold.rightHeld = true;
    f.menu.update(hold, 2);
    f.menu.update(hold, 6);
    CHECK(AudioSlider::value(f.menu.config().audio.musicVolume) == 136);
    CHECK(f.settings.audioPreviews.size() == 2);
    CHECK(std::ranges::all_of(f.settings.audioPreviews,
                              [](const AudioConfig& audio) { return audio.masterVolume == 1; }));
    CHECK(f.settings.writes == 0);
    f.back();
    CHECK(f.settings.writes == 1);
    CHECK(f.menu.page() == SettingsMenu::Page::Root);
}

TEST_CASE("audio sliders use timed native samples instead of a cursor tick per volume change",
          "[settings][audio-samples]") {
    Fixture f;
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Audio);
    MenuInput held;
    held.rightHeld = true;
    CHECK(f.menu.update(held, 6).action == MenuAction::None);
    CHECK(f.menu.audioSample().empty());
    f.down();
    CHECK(f.menu.update({}, 53).action == MenuAction::None);
    CHECK(f.menu.audioSample().empty()); // exactly 60 elapsed menu ticks
    f.menu.update({}, 1);
    CHECK(f.menu.audioSample() == "S_WARN");
    f.menu.update({}, 60);
    CHECK(f.menu.audioSample().empty());
    f.menu.update({}, 1);
    CHECK(f.menu.audioSample() == "S_PICKUPMAGIC");
    f.menu.update(held, 127); // effects 128 -> 255: no cursor or bound cue
    CHECK(f.menu.audioSample().empty());
    f.menu.update(held, 1);
    CHECK(f.menu.audioSample() == "S_VOLMOVE");
    f.menu.update(held, 15);
    CHECK(f.menu.audioSample().empty());
    f.menu.update(held, 1);
    CHECK(f.menu.audioSample() == "S_VOLMOVE");
    f.back();
    f.menu.update({}, 100);
    CHECK(f.menu.audioSample().empty());
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Audio);
    f.menu.update({}, 100);
    CHECK(f.menu.audioSample().empty()); // music row never emits effects samples
}

TEST_CASE("Video preview requires confirmation and uses a wall-clock rollback deadline",
          "[settings][graphics]") {
    Fixture f;
    GameConfig active = f.config;
    f64 now = 100;
    s32 previews = 0;
    f.settings.video = [&](const auto& next) {
        ++previews;
        active = next;
        return true;
    };
    f.menu.setClock([&] { return now; });
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, SettingsMenu::Scope::Level);
    f.choose(4);
    f.right();
    CHECK(active.display.vsync);
    f.down();
    f.down();
    f.down(); // skips unavailable resolution, window mode and display, reaches Depth of Field
    CHECK(f.menu.menu().definition().items[7].text == "Depth of Field");
    f.right();
    CHECK(f.menu.config().display.depthOfField);
    CHECK_FALSE(active.display.depthOfField);
    f.down();
    REQUIRE(f.menu.menu().selection() == 8);
    CHECK(f.menu.menu().definition().items[8].text == "Bloom");
    f.right();
    CHECK(f.menu.config().display.bloom);
    CHECK_FALSE(active.display.bloom);
    f.down();
    REQUIRE(f.menu.menu().selection() == 9);
    CHECK(f.menu.menu().definition().items[9].text == "Ambient Occlusion");
    f.right();
    CHECK(f.menu.config().display.ambientOcclusion);
    CHECK_FALSE(active.display.ambientOcclusion);
    f.down();
    REQUIRE(f.menu.menu().selection() == 10);
    CHECK(f.menu.menu().definition().items[10].text == "Texture Filtering");
    CHECK(f.menu.menu().definition().items[10].value == "8x Anisotropic");
    f.right();
    CHECK(f.menu.config().display.textureFiltering == 16);
    CHECK(active.display.textureFiltering == 8);
    f.down();
    REQUIRE(f.menu.menu().selection() == 11);
    CHECK(f.menu.menu().definition().items[11].text == "Smooth Sprites");
    CHECK(f.menu.menu().definition().items[11].value == "Off");
    f.right();
    CHECK(f.menu.config().display.smoothSprites);
    CHECK_FALSE(active.display.smoothSprites);
    f.down();
    REQUIRE(f.menu.menu().selection() == 12);
    f.select();
    REQUIRE(f.menu.menu().definition().items.size() == 2);
    CHECK(f.menu.menu().selection() == 1); // default to Revert, not Save
    CHECK_FALSE(active.display.vsync);
    CHECK(active.display.depthOfField);
    CHECK(active.display.bloom);
    CHECK(active.display.ambientOcclusion);
    CHECK(active.display.textureFiltering == 16);
    CHECK(active.display.smoothSprites);
    CHECK(f.config.display.vsync);
    CHECK(f.settings.writes == 0);
    CHECK(previews == 1);
    CHECK(f.menu.menu().definition().body[1] == "Reverting in 15 seconds");
    SECTION("Save confirms the preview") {
        MenuInput left;
        left.left = true;
        f.menu.update(left, 1);
        f.select();
        CHECK_FALSE(f.config.display.vsync);
        CHECK(f.config.display.depthOfField);
        CHECK(f.config.display.bloom);
        CHECK(f.config.display.ambientOcclusion);
        CHECK(f.config.display.textureFiltering == 16);
        CHECK(f.config.display.smoothSprites);
        CHECK(f.settings.writes == 1);
        now += 20;
        f.release();
        CHECK_FALSE(active.display.vsync);
    }
    SECTION("Timeout does not depend on game ticks") {
        now += 14.2;
        f.menu.update({}, 0);
        CHECK(f.menu.menu().definition().body[1] == "Reverting in 1 second");
        CHECK_FALSE(active.display.vsync);
        now += 0.8;
        f.menu.update({}, 0);
        CHECK(active.display.vsync);
        CHECK(previews == 2);
        CHECK(f.settings.writes == 0);
        CHECK(f.menu.menu().definition().items.size() == 14);
        CHECK_FALSE(active.display.smoothSprites);
        CHECK(active.display.textureFiltering == 8);
        CHECK_FALSE(active.display.depthOfField);
        CHECK_FALSE(active.display.ambientOcclusion);
        CHECK_FALSE(active.display.bloom);
    }
    SECTION("Back cancels the trial") {
        f.back();
        CHECK_FALSE(active.display.smoothSprites);
        CHECK(active.display.textureFiltering == 8);
        CHECK(active.display.vsync);
        CHECK_FALSE(active.display.bloom);
        CHECK(f.settings.writes == 0);
    }
    SECTION("Closing the owner restores the saved configuration") {
        f.menu.close();
        CHECK_FALSE(active.display.smoothSprites);
        CHECK(active.display.textureFiltering == 8);
        CHECK(active.display.vsync);
        CHECK_FALSE(active.display.bloom);
        CHECK(f.settings.writes == 0);
    }
}

TEST_CASE("Video defaults stay staged and the action row navigates horizontally",
          "[settings][graphics]") {
    Fixture f;
    f.config.display.vsync = false;
    f.config.display.depthOfField = true;
    f.config.display.bloom = true;
    f.config.display.ambientOcclusion = true;
    f.config.display.textureFiltering = 16;
    f.config.display.smoothSprites = true;
    f.config.audio.effectsVolume = 0.25f;
    f.menu.open(f.config, &f.strings, nullptr, f.painter, {}, {}, SettingsMenu::Scope::Level);
    f.choose(4);
    f.down();
    f.down();
    f.down();
    f.down();
    f.down();
    f.down();
    f.down();
    f.down();
    REQUIRE(f.menu.menu().selection() == 12);
    f.right();
    REQUIRE(f.menu.menu().selection() == 13);
    f.select();
    CHECK(f.menu.config().display.vsync);
    CHECK_FALSE(f.menu.config().display.depthOfField);
    CHECK_FALSE(f.menu.config().display.bloom);
    CHECK_FALSE(f.menu.config().display.ambientOcclusion);
    CHECK(f.menu.config().display.textureFiltering == 8);
    CHECK_FALSE(f.menu.config().display.smoothSprites);
    CHECK(f.menu.config().audio.effectsVolume == 0.25f);
    CHECK_FALSE(f.config.display.vsync);
    MenuInput up;
    up.up = true;
    f.menu.update(up, 1);
    CHECK(f.menu.menu().selection() == 11); // last enabled setting: Smooth Sprites
    f.down();
    f.right();
    REQUIRE(f.menu.menu().selection() == 13); // the row is Apply and Restore Defaults alone
    f.right();
    REQUIRE(f.menu.menu().selection() == 12);
    f.back();
    CHECK(f.menu.page() == SettingsMenu::Page::Root);
    CHECK_FALSE(f.menu.config().display.vsync);
    CHECK(f.settings.writes == 0);
}

TEST_CASE("Video rollback preserves a manually resized window", "[settings][graphics]") {
    Fixture f;
    GameConfig active = f.config;
    f.settings.fail = true; // nothing is saved
    f.settings.displayOptions = {{1920, 1080}, {{1920, 1080}}, {1152, 700}};
    f.settings.video = [&](const auto& next) {
        active = next;
        return true;
    };
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, SettingsMenu::Scope::Level);
    f.choose(4);
    CHECK(f.menu.menu().definition().items[4].value == "1152 x 700");
    f.down();
    f.down();
    f.down();
    REQUIRE(f.menu.menu().selection() == 5);
    f.right();
    f.down();
    f.down();
    f.down();
    f.down();
    f.down();
    f.down(); // past the post effects, Texture Filtering and Smooth Sprites to Apply
    f.select();
    REQUIRE(active.display.windowMode == WindowMode::Fullscreen);
    f.back();
    CHECK(active.display.windowMode == WindowMode::Windowed);
    CHECK(active.display.windowWidth == 1152);
    CHECK(active.display.windowHeight == 700);
}

TEST_CASE("Video font and columns reserve the widest choices before selecting them",
          "[settings][graphics]") {
    Fixture f;
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = 32; c <= 126; ++c) {
        glyphs.push_back({c, 24, 0, 0});
    }
    const auto font = BitmapFont::fromGlyphs(32, 12, std::move(glyphs));
    f.painter.setFont(&font, &f.texture);
    f.settings.fail = true; // nothing is saved
    f.settings.displayOptions = {{1920, 1080}, {{1280, 896}, {1920, 1080}}};
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, MenuDefinition::parchment(),
                SettingsMenu::Scope::Level);
    f.choose(4);
    const auto initial = f.menu.menu().definition();
    CHECK(initial.scale < 0.7f);
    const auto unchanged = [&] {
        const auto& current = f.menu.menu().definition();
        CHECK(current.scale == initial.scale);
        CHECK(current.valueX == initial.valueX);
        CHECK(current.valueWidth == initial.valueWidth);
    };
    f.right();
    unchanged();
    f.down();
    for (s32 i = 0; i < 3; ++i) {
        f.right();
        unchanged();
    }
    f.down();
    f.down(); // skips resolution while windowed
    REQUIRE(f.menu.menu().selection() == 5);
    for (s32 i = 0; i < 3; ++i) {
        f.right();
        unchanged();
    }
    f.down();
    f.down();
    f.down();
    f.down();
    f.down(); // past the post effects and Texture Filtering to Apply
    f.right();
    f.select(); // Restore Defaults
    unchanged();
}

TEST_CASE("Restore Defaults previews the default window size and thirty fps before saving",
          "[settings][graphics]") {
    Fixture f;
    f.config.display.maxFrameRate = 60;
    f.config.timing.gameplayFrameRate = 60;
    Extent2D actualSize{1600, 1000}; // saved dimensions still say 1280x896
    s32 resizes = 0;
    f.settings.displayOptions = {{1920, 1080}, {{1920, 1080}}, actualSize};
    f.settings.video = [&](const auto& next) {
        if (!next.display.matchesWindow(WindowMode::Windowed, actualSize)) {
            actualSize = {next.display.windowWidth, next.display.windowHeight};
            ++resizes;
        }
        return true;
    };
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, SettingsMenu::Scope::Level);
    f.choose(4);
    for (s32 i = 0; i < 9; ++i) {
        f.down();
    }
    REQUIRE(f.menu.menu().selection() == 12);
    f.right();
    f.select();
    CHECK(f.menu.config().display.windowMode == WindowMode::Windowed);
    CHECK(f.menu.config().display.windowWidth == 1280);
    CHECK(f.menu.config().display.windowHeight == 896);
    CHECK(f.menu.config().display.maxFrameRate == 30);
    CHECK(f.menu.config().timing.gameplayFrameRate == 30);
    CHECK(actualSize == Extent2D{1600, 1000});
    CHECK(f.settings.writes == 0);
    MenuInput left;
    left.left = true;
    f.menu.update(left, 1);
    f.select(); // Apply
    CHECK(actualSize == Extent2D{1280, 896});
    CHECK(resizes == 1);
    CHECK(f.settings.writes == 0);
    SECTION("Save keeps defaults") {
        f.menu.update(left, 1);
        f.select();
        CHECK(f.settings.writes == 1);
        CHECK(f.config.display.maxFrameRate == 30);
        CHECK(f.config.timing.gameplayFrameRate == 30);
        CHECK(actualSize == Extent2D{1280, 896});
    }
    SECTION("Revert restores the manually resized window") {
        f.back();
        CHECK(actualSize == Extent2D{1600, 1000});
        CHECK(f.menu.config().timing.gameplayFrameRate == 60);
        CHECK(resizes == 2);
        CHECK(f.settings.writes == 0);
    }
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

TEST_CASE("Controls is available in every scope and opening it does not mutate bindings",
          "[settings]") {
    Fixture f;
    for (const auto scope :
         {SettingsMenu::Scope::Title, SettingsMenu::Scope::Tower, SettingsMenu::Scope::Level}) {
        f.menu.open(f.config, &f.strings, nullptr, f.painter, {}, {}, scope);
        const auto& items = f.menu.menu().definition().items;
        const auto controls = std::ranges::find(items, 3, &MenuItem::code);
        REQUIRE(controls != items.end());
        CHECK(controls->enabled);
        const auto bindings = f.config.toJson();
        f.menu.menu().focus(static_cast<usize>(controls - items.begin()));
        f.select();
        CHECK(f.menu.page() == SettingsMenu::Page::Controls);
        CHECK(f.menu.menu().definition().items.size() == 4);
        CHECK(f.config.toJson() == bindings);
        CHECK(f.settings.writes == 0);
    }
}

TEST_CASE("controls capture waits for release and binds the chosen player's mouse buttons",
          "[settings][controls][mouse]") {
    Fixture f;
    Input physical;
    const auto step = [&] {
        auto input = readMenuInput(physical, f.config.menu);
        f.menu.update(input, 1);
    };
    const auto& root = f.menu.menu().definition().items;
    const auto found = std::ranges::find(root, 3, &MenuItem::code);
    REQUIRE(found != root.end());
    f.menu.menu().focus(static_cast<usize>(found - root.begin()));
    f.select();
    f.menu.menu().focus(2);
    f.select(); // player 3, not the settings menu's owner
    f.right();  // automatic -> keyboard
    REQUIRE(f.config.controls[2].device.empty());
    CHECK(f.menu.menu().definition().items[0].value == "Mouse and Keyboard");
    f.focus(4); // Quick Attack, by its action rather than its row under the scheme
    physical.beginPoll();
    physical.setKey(Key::Enter, true);
    step();
    REQUIRE_FALSE(f.config.controls[2].customized);
    physical.beginPoll();
    step(); // opening press remains held
    REQUIRE_FALSE(f.config.controls[2].customized);
    physical.beginPoll();
    physical.setKey(Key::Enter, false);
    step();
    physical.beginPoll();
    physical.setKey(Key::MouseRight, true);
    step();
    CHECK_FALSE(f.config.controls[2].customized);
    f.applyControls();
    CHECK(f.config.controls[2].play.attack == std::vector{Key::MouseRight});
    CHECK(f.config.controls[2].customized);
    CHECK_FALSE(f.config.controls[0].customized);
    // A failed write must not replace the live profile.
    physical.beginPoll();
    physical.setKey(Key::MouseRight, false);
    step();
    f.settings.fail = true;
    f.focus(4);
    f.select();
    physical.beginPoll();
    step();
    physical.beginPoll();
    physical.setKey(Key::F3, true);
    step();
    f.applyControls();
    CHECK(f.config.controls[2].play.attack == std::vector{Key::MouseRight});
    CHECK(f.menu.menu().definition().body == std::vector<std::string>{"COULD NOT SAVE SETTINGS"});
}

TEST_CASE("controls lists a hot-plugged controller and cancels capture without changing bindings",
          "[settings][controls]") {
    Fixture f;
    Input physical;
    f.menu.menu().focus(4);
    f.select(); // Controls in the title scope
    REQUIRE(f.menu.page() == SettingsMenu::Page::Controls);
    f.select(); // player 1
    PadSnapshot pad;
    pad.connected = true;
    pad.name = "Linux USB Gamepad";
    pad.guid = "test-guid";
    physical.setPad(6, pad);
    f.menu.update(readMenuInput(physical, f.config.menu), 1);
    f.right();
    f.right();
    CHECK(f.config.controls[0].device.empty());
    f.applyControls();
    CHECK(f.config.controls[0].device == "test-guid");
    CHECK(f.menu.menu().definition().items[0].value == "Linux USB Gamepad 1");
    CHECK(f.menu.menu().definition().items[1].value == "Left Stick Up");
    CHECK(f.menu.menu().definition().items[2].value == "Left Stick Down");
    CHECK(f.menu.menu().definition().items[3].value == "Left Stick Left");
    CHECK(f.menu.menu().definition().items[4].value == "Left Stick Right");
    f.menu.menu().focus(5);
    f.select();
    physical.beginPoll();
    physical.setKey(Key::Escape, true);
    f.menu.update(readMenuInput(physical, f.config.menu), 1);
    CHECK_FALSE(f.config.controls[0].customized);
    physical.beginPoll();
    physical.setKey(Key::Escape, false);
    physical.setPad(6, {});
    f.menu.update(readMenuInput(physical, f.config.menu), 1);
    CHECK(f.menu.menu().definition().items[0].value == "Disconnected");
}

TEST_CASE("control drafts apply explicitly, discard on Back, and restore actual defaults",
          "[settings][controls]") {
    Fixture f;
    f.menu.menu().focus(4);
    f.select();
    f.select();
    const auto original = f.config.toJson();
    f.right(); // Player 1: Automatic -> keyboard; None is never offered.
    REQUIRE(f.menu.menu().definition().items[0].value == "Mouse and Keyboard");
    CHECK(f.config.toJson() == original);
    f.right();
    CHECK(f.menu.menu().definition().items[0].value == "Automatic");
    f.right();
    SECTION("Back discards the pending device") {
        f.back();
        CHECK(f.settings.writes == 0);
        CHECK(f.config.toJson() == original);
        f.select();
        CHECK(f.menu.menu().definition().items[0].value == "Automatic");
    }
    SECTION("Apply saves and Defaults remain staged") {
        f.applyControls();
        CHECK(f.settings.writes == 1);
        CHECK(f.config.controls[0].device == "keyboard");
        f.right(); // Apply -> Restore Defaults
        f.select();
        CHECK(f.settings.writes == 1);
        CHECK_FALSE(f.config.controls[0].customized);
        f.applyControls();
        CHECK(f.settings.writes == 2);
        CHECK(f.config.controls[0].play.padUp == std::vector{PadButton::LeftStickUp});
        CHECK(f.config.controls[0].menu.padSelect == MenuBindings{}.padSelect);
    }
}

TEST_CASE("controller default rows expose stick directions and potion gestures before Apply",
          "[settings][controls]") {
    Fixture f;
    Input physical;
    PadSnapshot pad;
    pad.connected = true;
    pad.guid = "xinput";
    pad.name = "XInput Controller";
    pad.rumbleSupported = true;
    physical.setPad(0, pad);
    f.menu.menu().focus(4);
    f.select();
    f.select();
    f.menu.update(readPlayerMenuInput(physical, f.config, 0), 1);
    f.right();
    f.right(); // Player 1 skips None.
    REQUIRE(f.menu.menu().definition().items[0].value == "XInput Controller 1");
    CHECK(f.menu.menu().definition().items[1].value == "Left Stick Up");
    // The actions stand in one column after the device: the potion gestures on their rows.
    CHECK(f.menu.menu().definition().items[9].text == "Throw Potion");
    CHECK(f.menu.menu().definition().items[9].value == "X (hold)");
    CHECK(f.menu.menu().definition().items[10].value == "X (double tap)");
    CHECK(f.settings.writes == 0);
    // An unplugged device cannot remove the last available input route.
    physical.setPad(0, {});
    f.menu.update(readPlayerMenuInput(physical, f.config, 0), 1);
    f.applyControls();
    CHECK(f.settings.writes == 0);
    CHECK(f.config.controls[0].device.empty());
    CHECK(f.menu.menu().definition().body == std::vector<std::string>{"COULD NOT SAVE SETTINGS"});
}

TEST_CASE("control rows scroll in one column above a horizontal action row",
          "[settings][controls][mouse][scroll-menu]") {
    Fixture f;
    const bool rumble = GENERATE(false, true);
    Input physical;
    PadSnapshot pad;
    pad.connected = true;
    pad.guid = "layout-pad";
    pad.rumbleSupported = rumble;
    physical.setPad(0, pad);
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = ' '; c <= '~'; ++c) {
        glyphs.push_back({c, 8, 0, 0});
    }
    f.font = BitmapFont::fromGlyphs(32, 8, std::move(glyphs));
    f.menu.menu().focus(4);
    f.select();
    f.select();
    f.menu.update(readPlayerMenuInput(physical, f.config, 0), 1);
    const auto& menu = f.menu.menu();
    const auto& definition = menu.definition();
    // The device and the keyboard's scheme (Automatic keeps the keyboard), every action, the
    // gestures and the rumble row when there is one, then Apply and Restore Defaults in a
    // row of their own.
    const auto actions = controlActions().size();
    const usize rows = 2 + actions + 1 + (rumble ? 1 : 0);
    REQUIRE(definition.items.size() == rows + 2);
    CHECK(definition.visibleRows == ControlSettings::kVisibleRows);
    REQUIRE(definition.tailPositions.size() == 2);
    const usize apply = rows;
    CHECK(definition.items[apply].text == "Apply");
    CHECK(definition.items[apply + 1].text == "Restore Defaults");
    CHECK(menu.itemY(apply) == menu.itemY(apply + 1));
    CHECK(menu.itemArea(apply).x + menu.itemArea(apply).width < menu.itemArea(apply + 1).x);
    CHECK(std::abs((menu.itemArea(apply).x + menu.itemArea(apply + 1).x +
                    menu.itemArea(apply + 1).width) -
                   512) <= 2 * definition.scale * 8 + 2);
    // Only the window's rows are in view; the rest wait below it, with no area to click.
    CHECK(menu.scroll() == 0);
    CHECK(menu.windowRows() == ControlSettings::kVisibleRows);
    CHECK(menu.shown(0));
    CHECK(menu.shown(static_cast<usize>(ControlSettings::kVisibleRows - 1)));
    CHECK_FALSE(menu.shown(static_cast<usize>(ControlSettings::kVisibleRows)));
    CHECK(menu.itemArea(static_cast<usize>(ControlSettings::kVisibleRows)).width == 0);
    CHECK(menu.itemY(static_cast<usize>(ControlSettings::kVisibleRows - 1)) < menu.itemY(apply));
    for (usize i = 1; i < static_cast<usize>(ControlSettings::kVisibleRows); ++i) {
        CHECK(menu.itemY(i) == menu.itemY(i - 1) + 18); // the editor's own row step
    }
    // Walking down scrolls the window along; the wheel does the same, a row a notch.
    for (s32 i = 0; i < ControlSettings::kVisibleRows; ++i) {
        f.down();
    }
    CHECK(menu.selection() == ControlSettings::kVisibleRows);
    CHECK(menu.scroll() == 1);
    CHECK(menu.shown(static_cast<usize>(ControlSettings::kVisibleRows)));
    CHECK_FALSE(menu.shown(0));
    MenuInput wheel;
    wheel.pointerScroll = -1.0f;
    f.menu.update(wheel, 1);
    CHECK(menu.selection() == ControlSettings::kVisibleRows); // the wheel moves the window alone
    CHECK(menu.scroll() == 2);
    wheel.pointerScroll = 3.0f;
    f.menu.update(wheel, 1);
    CHECK(menu.selection() == ControlSettings::kVisibleRows);
    CHECK(menu.scroll() == 0);
    f.down(); // a key brings the window back to the selection
    CHECK(menu.selection() == ControlSettings::kVisibleRows + 1);
    CHECK(menu.scroll() == 2);
    f.menu.update(wheel, 1);
    CHECK(menu.scroll() == 0);
    f.menu.menu().focus(static_cast<usize>(ControlSettings::kVisibleRows - 2));
    CHECK(menu.scroll() == 0);
    f.menu.menu().scrollWindow(2);
    CHECK(menu.scroll() == 2); // still in the window: it does not scroll back
    // A row chosen in the window's middle leaves the window where it is when the page is
    // written again for it, instead of bringing the row to the window's edge.
    f.focus(107); // the gestures row, near the column's end
    const s32 held = menu.scroll();
    CHECK(held > 2);
    MenuInput upOne;
    upOne.up = true;
    f.menu.update(upOne, 1);
    f.menu.update(upOne, 1);
    CHECK(menu.scroll() == held);
    const s32 chosen = menu.selection();
    f.select(); // a binding row: the capture begins and the page is written again
    CHECK(menu.selection() == chosen);
    CHECK(menu.scroll() == held);
    // The bar stands at the right hand of the value column.
    CHECK(menu.columnRight() > definition.valueX + definition.valueWidth);
    f.menu.update({}, 600); // the capture times out; the window still stands
    CHECK(menu.scroll() == held);
    // The action row is reached from the column's end and leads back to its ends.
    f.focus(104);
    REQUIRE(menu.selection() == static_cast<s32>(apply));
    f.right();
    CHECK(menu.selection() == static_cast<s32>(apply) + 1);
    f.right();
    CHECK(menu.selection() == static_cast<s32>(apply));
    MenuInput up;
    up.up = true;
    f.menu.update(up, 1);
    CHECK(menu.selection() == static_cast<s32>(rows) - 1);
    CHECK(menu.shown(rows - 1));
    f.down();
    CHECK(menu.selection() == static_cast<s32>(apply));
    f.down();
    CHECK(menu.selection() == 0);
    CHECK(menu.scroll() == 0);
}

TEST_CASE("a player's potion gestures are a row of their own and are kept with the profile",
          "[settings][controls][gestures]") {
    Fixture f;
    f.menu.menu().focus(4);
    f.select();
    f.select(); // player 1, the keyboard
    f.right();  // Automatic -> keyboard
    const auto& items = f.menu.menu().definition().items;
    const auto gestures = std::ranges::find(items, 107, &MenuItem::code);
    REQUIRE(gestures != items.end());
    CHECK(gestures->text == "Hold / Double Tap");
    CHECK(gestures->value == "Off");
    const auto throwRow = std::ranges::find(items, 8, &MenuItem::code);
    REQUIRE(throwRow != items.end());
    CHECK(throwRow->value == "Q");
    f.focus(107);
    f.right();
    CHECK(std::ranges::find(items, 107, &MenuItem::code)->value == "On");
    CHECK_FALSE(f.config.controls[0].play.keyboardMagicGestures);
    f.applyControls();
    CHECK(f.config.controls[0].play.keyboardMagicGestures);
    CHECK(f.config.controls[0].play.padMagicGestures);
    CHECK(f.config.controls[0].customized);
    // A profile keeps its own gestures through a settings file.
    GameConfig restored;
    restored.mergeJson(f.config.toJson());
    CHECK(restored.controls[0].play.keyboardMagicGestures);
    CHECK_FALSE(restored.controls[1].play.keyboardMagicGestures);
}

TEST_CASE("disabling another player's device stays pending and old navigation survives Apply",
          "[settings][controls]") {
    Fixture f;
    Input physical;
    PadSnapshot pad;
    pad.connected = true;
    pad.guid = "controller";
    physical.setPad(1, pad);
    f.menu.menu().focus(4);
    f.select();
    f.menu.menu().focus(1);
    f.select();
    f.menu.update(readPlayerMenuInput(physical, f.config, 1), 1);
    f.right(); // keyboard
    f.right(); // None (allowed for player 2)
    REQUIRE(f.menu.menu().definition().items[0].value == "None");
    CHECK(f.config.controls[1].device.empty());
    f.applyControls();
    REQUIRE(f.config.controls[1].device == "none");
    physical.beginPoll();
    pad.buttons[static_cast<usize>(PadButton::DpadRight)] = true;
    physical.setPad(1, pad);
    // The newly unassigned owner supplies no mapped input, but the editor keeps its
    // original device route until it closes.
    f.menu.update(readPlayerMenuInput(physical, f.config, 1), 1);
    const auto selected = static_cast<usize>(f.menu.menu().selection());
    CHECK(f.menu.menu().definition().items[selected].code == 102); // Restore Defaults
}

TEST_CASE("multiplayer radio choices use retail labels and persist only successful writes",
          "[settings][multiplayer]") {
    // GC OPTMENU_MULTIPLAYER entries at 0x8011ED7C: strings at 0x80347568,
    // 0x80113900 and 0x8011391C; the selected radio is optglobals + 0x9C.
    Fixture f;
    f.down();
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Game);
    REQUIRE(f.menu.menu().definition().items[1].enabled);
    f.down();
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Multiplayer);
    CHECK(f.menu.menu().selection() == 0);
    const auto checkLabels = [&] {
        const auto& rows = f.menu.menu().definition().items;
        REQUIRE(rows.size() == 3);
        CHECK(rows[0].text == "Normal");
        CHECK(rows[1].text == "Shots Stun Other Players");
        CHECK(rows[2].text == "Shots Hurt Other Players");
    };
    checkLabels();
    CHECK(f.menu.menu().definition().items[0].markedPart == 1);
    f.down();
    f.settings.fail = true;
    f.select();
    CHECK(f.settings.writes == 0);
    CHECK(f.config.multiplayer.mode == MultiplayerMode::Normal);
    CHECK(f.menu.config().multiplayer.mode == MultiplayerMode::Normal);
    CHECK(f.menu.menu().definition().items[0].markedPart == 1);
    CHECK(f.menu.menu().definition().items[1].markedPart == 0);
    f.settings.fail = false;
    f.select();
    CHECK(f.config.multiplayer.mode == MultiplayerMode::Stun);
    CHECK(f.settings.writes == 1);
    CHECK(f.menu.menu().definition().items[1].markedPart == 1);
    f.down();
    f.select();
    CHECK(f.config.multiplayer.mode == MultiplayerMode::Hurt);
    CHECK(f.menu.menu().definition().items[2].markedPart == 1);
    CHECK(f.menu.menu().definition().items[1].markedPart == 0);
    f.back();
    CHECK(f.menu.page() == SettingsMenu::Page::Game);
    CHECK(f.menu.menu().selection() == 1);
    f.select();
    CHECK(f.menu.page() == SettingsMenu::Page::Multiplayer);
    CHECK(f.menu.menu().selection() == 2);
    checkLabels();
}

TEST_CASE("the Video page offers the renderer from the title, needing a restart, and greys "
          "the scene effects OpenGL does not offer",
          "[settings][graphics][renderer]") {
    // The renderer choice and its notice are the user-requested PC feature, not retail text.
    Fixture f;
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = ' '; c <= '~'; ++c) {
        glyphs.push_back({c, 8, 0, 0});
    }
    f.font = BitmapFont::fromGlyphs(32, 8, std::move(glyphs));
    f.painter.setFont(&f.font, &f.texture);
    f.choose(4);
    const auto& items = f.menu.menu().definition().items;
    REQUIRE(items.size() == 14);
    CHECK(items[0].code == 14);
    CHECK(items[0].text == "Renderer");
    CHECK(items[0].value == "Vulkan");
    CHECK(items[0].enabled);
    CHECK(items[7].text == "Depth of Field");
    CHECK(items[7].enabled);
    CHECK(items[8].enabled);
    CHECK(items[9].enabled);
    CHECK_FALSE(f.menu.restartNeeded());
    const auto noticeDrawn = [&] {
        test::FakeRenderDevice device;
        Canvas canvas;
        canvas.begin(device, Mat4{1});
        f.menu.draw(canvas, f.painter, {});
        canvas.end();
        return std::ranges::any_of(device.draws, [](const auto& draw) {
            return std::ranges::any_of(draw.vertices,
                                       [](const auto& vertex) { return vertex.position.y >= 340; });
        });
    };
    CHECK_FALSE(noticeDrawn());
    f.right();
    CHECK(f.menu.config().display.renderer == GraphicsApi::OpenGl);
    CHECK(items[0].value == "OpenGL");
    CHECK(f.menu.restartNeeded());
    CHECK(noticeDrawn()); // the choice takes effect at the next start, and says so
    // Scene post-processing is Vulkan's alone: those rows are greyed and skipped.
    CHECK_FALSE(items[7].enabled);
    CHECK_FALSE(items[8].enabled);
    CHECK_FALSE(items[9].enabled);
    for (s32 i = 0; i < 4; ++i) {
        f.down();
    }
    CHECK(f.menu.menu().selection() == 10); // Texture Filtering, past the greyed three
    MenuInput left;
    left.left = true;
    f.focus(14);
    f.menu.update(left, 1);
    CHECK(f.menu.config().display.renderer == GraphicsApi::Vulkan);
    CHECK(items[7].enabled);
    CHECK_FALSE(f.menu.restartNeeded());
    CHECK(f.settings.writes == 0); // staged until Apply, like every Video row
    // Restore Defaults returns to Vulkan.
    f.right();
    REQUIRE(f.menu.config().display.renderer == GraphicsApi::OpenGl);
    f.focus(6);
    f.select();
    CHECK(f.menu.config().display.renderer == GraphicsApi::Vulkan);
    // Running on OpenGL, the saved choice shows as it is, with no restart needed.
    f.config.display.renderer = GraphicsApi::OpenGl;
    f.settings.fail = true; // nothing is saved
    f.settings.drawingApi = GraphicsApi::OpenGl;
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, SettingsMenu::Scope::Title);
    f.choose(4);
    CHECK(f.menu.menu().definition().items[0].value == "OpenGL");
    CHECK_FALSE(f.menu.menu().definition().items[7].enabled);
    CHECK_FALSE(f.menu.restartNeeded());
    CHECK_FALSE(noticeDrawn());
    // In play, the renderer cannot be swapped under the level: the row is greyed, skipped,
    // and left alone by Restore Defaults.
    for (const auto scope : {SettingsMenu::Scope::Tower, SettingsMenu::Scope::Level}) {
        f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, scope);
        f.choose(4);
        const auto& rows = f.menu.menu().definition().items;
        CHECK(rows[0].text == "Renderer");
        CHECK_FALSE(rows[0].enabled);
        CHECK(f.menu.menu().selection() == 1);
        f.focus(14);
        f.right();
        CHECK(f.menu.config().display.renderer == GraphicsApi::OpenGl);
        f.focus(6);
        f.select();
        CHECK(f.menu.config().display.renderer == GraphicsApi::OpenGl);
        f.back();
        f.back();
    }
}

TEST_CASE("the Video page offers the display when more than one is attached",
          "[settings][graphics][display]") {
    Fixture f;
    f.settings.displayOptions = {{1920, 1080}, {{1920, 1080}}, {1280, 896}, {"Left", "Right"}, 1};
    f.settings.video = [](const auto&) { return true; };
    f.menu.open(f.config, &f.strings, &f.settings, f.painter, {}, {}, SettingsMenu::Scope::Title);
    f.choose(4);
    REQUIRE(f.menu.page() == SettingsMenu::Page::Graphics);
    const auto& items = f.menu.menu().definition().items;
    const auto row = std::ranges::find(items, 13, &MenuItem::code);
    REQUIRE(row != items.end());
    CHECK(row->text == "Display");
    CHECK(row->enabled);
    CHECK(row->value == "2"); // the window is on the second, nothing chosen yet
    CHECK(f.menu.config().display.monitor == -1);
    f.focus(13);
    f.right();
    CHECK(f.menu.config().display.monitor == 0);
    CHECK(std::ranges::find(items, 13, &MenuItem::code)->value == "1");
    f.right();
    CHECK(f.menu.config().display.monitor == 1);
    MenuInput left;
    left.left = true;
    f.menu.update(left, 1);
    f.menu.update(left, 1);
    CHECK(f.menu.config().display.monitor == 1); // wrapped round the two
    // One display alone offers no choice: the row is greyed and skipped.
    Fixture single;
    single.choose(4);
    const auto& alone = single.menu.menu().definition().items;
    const auto greyed = std::ranges::find(alone, 13, &MenuItem::code);
    REQUIRE(greyed != alone.end());
    CHECK_FALSE(greyed->enabled);
    CHECK(greyed->value == "1");
}

TEST_CASE("the Video page takes the display the running game offers as it is entered",
          "[settings][graphics][display]") {
    Fixture f;
    const auto displayRow = [&] {
        const auto& items = f.menu.menu().definition().items;
        const auto row = std::ranges::find(items, 13, &MenuItem::code);
        REQUIRE(row != items.end());
        return *row;
    };
    f.choose(4);
    REQUIRE(f.menu.page() == SettingsMenu::Page::Graphics);
    CHECK_FALSE(displayRow().enabled);
    // A second display attached while the menu is open shows once the page is entered again.
    CHECK(f.menu.config().display.windowWidth == 1280);
    f.settings.displayOptions = {{1920, 1080}, {{1920, 1080}}, {1600, 1000}, {"Left", "Right"}, 1};
    f.back();
    f.choose(4);
    REQUIRE(f.menu.page() == SettingsMenu::Page::Graphics);
    CHECK(displayRow().enabled);
    CHECK(displayRow().value == "2");
    CHECK(f.menu.config().display.windowWidth == 1600); // the window as it stands now
    CHECK(f.menu.config().display.windowHeight == 1000);
}

} // namespace
TEST_CASE("each player's rumble preference is staged applied and reset independently",
          "[settings][controls][rumble]") {
    Fixture f;
    Input physical;
    PadSnapshot pad;
    pad.connected = true;
    pad.guid = "feedback-pad";
    pad.rumbleSupported = true;
    physical.setPad(2, pad);
    f.menu.menu().focus(4);
    f.select();
    f.menu.menu().focus(2);
    f.select();
    f.menu.update(readPlayerMenuInput(physical, f.config, 2), 1);
    const auto toggle = [&] {
        const auto& items = f.menu.menu().definition().items;
        const auto row = std::ranges::find(items, 105, &MenuItem::code);
        REQUIRE(row != items.end());
        // Retail menu strings at 80113938/80113974.
        CHECK(row->text == "Rumble Feature");
        f.menu.menu().focus(static_cast<usize>(row - items.begin()));
        f.right();
    };
    toggle();
    CHECK(f.config.controls[2].rumble);
    f.back();
    CHECK(f.config.controls[2].rumble);
    f.select();
    toggle();
    f.applyControls();
    CHECK_FALSE(f.config.controls[2].rumble);
    CHECK(f.config.controls[0].rumble);
    f.right(); // Restore Defaults in the action bar
    f.select();
    CHECK_FALSE(f.config.controls[2].rumble);
    f.applyControls();
    CHECK(f.config.controls[2].rumble);
}

TEST_CASE("rumble is offered only for the controller assigned in the pending profile",
          "[settings][controls][rumble][rumble-capability]") {
    Fixture f;
    Input physical;
    PadSnapshot unsupported;
    unsupported.connected = true;
    unsupported.guid = "twins";
    unsupported.name = "XInput Controller";
    auto capable = unsupported;
    capable.rumbleSupported = true;
    physical.setPad(0, unsupported);
    physical.setPad(2, capable);
    f.choose(3); // Controls
    f.choose(2); // Player 3
    f.menu.update(readPlayerMenuInput(physical, f.config, 2), 1);
    const auto shown = [&] {
        return std::ranges::any_of(f.menu.menu().definition().items,
                                   [](const MenuItem& item) { return item.code == 105; });
    };
    REQUIRE(shown()); // Automatic routes Player 3 to the capable controller.
    f.right();
    REQUIRE(f.menu.menu().definition().items[0].value == "Mouse and Keyboard");
    CHECK_FALSE(shown());
    f.right();
    REQUIRE(f.menu.menu().definition().items[0].value == "None");
    CHECK_FALSE(shown());
    f.right();
    REQUIRE(f.menu.menu().definition().items[0].value == "XInput Controller 1");
    CHECK_FALSE(shown()); // A controller name or another controller's motors are insufficient.
    f.right();
    REQUIRE(f.menu.menu().definition().items[0].value == "XInput Controller 2");
    CHECK(shown());
    CHECK(f.settings.writes == 0);
    f.applyControls();
    CHECK(f.config.controls[2].device == "twins");
    CHECK(f.config.controls[2].occurrence == 1);
    physical.setPad(2, {});
    f.menu.update(readPlayerMenuInput(physical, f.config, 2), 1);
    CHECK(f.menu.menu().definition().items[0].value == "Disconnected");
    CHECK_FALSE(shown());
    CHECK(f.config.controls[2].rumble); // Availability never resets a saved preference.
}

TEST_CASE("rumble hardware changes preserve draft preferences and action bar focus",
          "[settings][controls][rumble][rumble-capability]") {
    Fixture f;
    const s32 player = GENERATE(0, 1, 2, 3);
    Input physical;
    PadSnapshot pad;
    pad.connected = true;
    pad.guid = "hotplug";
    pad.rumbleSupported = true;
    f.choose(3);
    f.choose(player);
    const auto update = [&] { f.menu.update(readPlayerMenuInput(physical, f.config, player), 1); };
    const auto shown = [&] {
        return std::ranges::any_of(f.menu.menu().definition().items,
                                   [](const MenuItem& item) { return item.code == 105; });
    };
    const auto selectedCode = [&] {
        return f.menu.menu().definition().items[static_cast<usize>(f.menu.menu().selection())].code;
    };
    update();
    CHECK_FALSE(shown()); // No connected device for Automatic yet.
    physical.setPad(player, pad);
    update();
    REQUIRE(shown());
    f.choose(105); // Stage Off, not yet saved.
    REQUIRE(f.config.controls[static_cast<usize>(player)].rumble);
    pad.rumbleSupported = false;
    physical.setPad(player, pad);
    update();
    CHECK_FALSE(shown());
    CHECK(selectedCode() == 100); // The removed toggle falls back to the device selector.
    for (const s32 code : {104, 102}) {
        f.focus(code);
        pad.rumbleSupported = true;
        physical.setPad(player, pad);
        update();
        REQUIRE(shown());
        CHECK(selectedCode() == code);
        const auto& items = f.menu.menu().definition().items;
        const auto row = std::ranges::find(items, 105, &MenuItem::code);
        REQUIRE(row != items.end());
        CHECK(row->value == "Off");
        physical.setPad(player, {});
        update();
        CHECK_FALSE(shown());
        CHECK(selectedCode() == code);
    }
    CHECK(f.settings.writes == 0);
    physical.setPad(player, pad);
    update();
    f.applyControls();
    CHECK_FALSE(f.config.controls[static_cast<usize>(player)].rumble);
    CHECK(f.settings.writes == 1);
}

TEST_CASE("the list of players in Controls reads at the choice pages' size",
          "[settings][controls]") {
    Fixture f;
    f.menu.menu().focus(4);
    f.select();
    REQUIRE(f.menu.page() == SettingsMenu::Page::Controls);
    // The players stand as the root's choices do: full size, centred on the screen.
    const auto& definition = f.menu.menu().definition();
    CHECK(definition.scale == 1.0f);
    CHECK(definition.cursorScale == 1.0f);
    CHECK(definition.x == -256);
    CHECK(definition.y == -1);
    CHECK(definition.itemPositions.empty());
    CHECK(definition.items.front().text == "Player 1");
    CHECK(definition.items.back().text == "Player 4");
    f.select();
    // A player's bindings keep their small rows, their own value column and a window of a
    // few rows over the Apply and Restore Defaults row.
    CHECK(definition.scale < SettingsRows::kChoiceScale);
    CHECK(definition.itemPositions.empty());
    CHECK(definition.visibleRows == ControlSettings::kVisibleRows);
    CHECK(definition.tailPositions.size() == 2);
    CHECK(definition.items.front().value == "Automatic");
}

TEST_CASE("Controls is heard: moves, choices and the depth of a step back",
          "[settings][controls]") {
    Fixture f;
    CHECK(f.menu.depth() == 0);
    f.menu.menu().focus(4);
    MenuInput select;
    select.select = true;
    CHECK(f.menu.update(select, 1).action == MenuAction::Choice);
    CHECK(f.menu.depth() == 1);
    MenuInput down;
    down.down = true;
    CHECK(f.menu.update(down, 1).action == MenuAction::Moved);
    f.release();
    f.menu.menu().focus(0);
    CHECK(f.menu.update(select, 1).action == MenuAction::Choice);
    CHECK(f.menu.depth() == 2);
    CHECK(f.menu.update(down, 1).action == MenuAction::Moved);
    f.release();
    MenuInput right;
    right.right = true;
    f.menu.menu().focus(0);
    CHECK(f.menu.update(right, 1).action == MenuAction::Moved); // the device turns like a value
    f.release();
    MenuInput back;
    back.back = true;
    CHECK(f.menu.update(back, 1).action == MenuAction::None);
    CHECK(f.menu.page() == SettingsMenu::Page::Controls);
    CHECK(f.menu.depth() == 1);
    f.release();
    CHECK(f.menu.update(back, 1).action == MenuAction::None);
    CHECK(f.menu.page() == SettingsMenu::Page::Root);
    CHECK(f.menu.depth() == 0);
    f.release();
    CHECK(f.menu.update(back, 1).action == MenuAction::Back);
}

TEST_CASE("Game Options turns the language through the tables the text directory holds",
          "[settings][language]") {
    const auto dir = test::scratchDirectory("settings-languages");
    writeTextFile(dir / "en.json",
                  R"({"language.name": "English", "settings.language": "Language",
                      "menu.gameOptions": "Game Options"})");
    writeTextFile(dir / "xx.json", R"({"language.name": "Testish"})");
    Fixture f;
    StringTable strings;
    REQUIRE(strings.load(dir, "en"));
    f.menu.open(f.config, &strings, &f.settings, f.painter, {}, {});
    f.choose(1);
    REQUIRE(f.menu.page() == SettingsMenu::Page::Game);
    const auto& items = f.menu.menu().definition().items;
    REQUIRE(items.back().text == "Language");
    CHECK(items.back().value == "English");
    f.focus(5);
    f.right();
    CHECK(f.config.text.language == "xx");
    CHECK(f.settings.writes == 1);
    CHECK(f.menu.menu().definition().items.back().value == "Testish");
    f.release();
    f.right();
    CHECK(f.config.text.language == "en");
    CHECK(f.settings.writes == 2);
    f.release();
    MenuInput left;
    left.left = true;
    f.menu.update(left, 1);
    CHECK(f.config.text.language == "xx");
    CHECK(f.settings.writes == 3);
    f.release();
    // Choosing the row turns it on as well; a failed write leaves the language alone.
    f.settings.fail = true;
    f.select();
    CHECK(f.config.text.language == "xx");
    CHECK(f.settings.writes == 3);
    // With one table there is nothing to turn to, and nothing is written.
    const auto lone = test::scratchDirectory("settings-one-language");
    writeTextFile(lone / "en.json", R"({"language.name": "English"})");
    StringTable one;
    REQUIRE(one.load(lone, "en"));
    f.settings.fail = false;
    f.config.text.language = "en";
    f.menu.open(f.config, &one, &f.settings, f.painter, {}, {});
    f.choose(1);
    f.focus(5);
    f.right();
    CHECK(f.config.text.language == "en");
    CHECK(f.settings.writes == 3);
}

TEST_CASE("a keyboard player's control scheme stands under its device, is turned and restored",
          "[settings][controls][robotron]") {
    Fixture f;
    f.menu.menu().focus(4);
    f.select();
    f.select(); // Player 1: the automatic profile holds the mouse and keyboard
    const auto& items = f.menu.menu().definition().items;
    REQUIRE(items.size() > 2);
    CHECK(items[0].code == 100);
    REQUIRE(items[1].code == 106);
    CHECK(items[1].text == "Control Scheme");
    CHECK(items[1].value == "Tank");
    f.focus(106);
    f.right();
    CHECK(f.menu.menu().definition().items[1].value == "Robotron");
    CHECK(f.config.controls[0].scheme == KeyboardScheme::Tank); // staged until Apply
    f.release();
    f.applyControls();
    CHECK(f.config.controls[0].scheme == KeyboardScheme::Robotron);
    CHECK(f.settings.writes == 1);
    f.focus(106);
    f.select(); // choosing the row turns it too
    CHECK(f.menu.menu().definition().items[1].value == "Tank");
    f.release();
    f.focus(102); // Restore Defaults
    f.select();
    CHECK(f.menu.menu().definition().items[1].value == "Tank");
    f.applyControls();
    CHECK(f.config.controls[0].scheme == KeyboardScheme::Tank);
    // A player on a controller alone has no scheme to choose.
    Input physical;
    PadSnapshot pad;
    pad.connected = true;
    pad.guid = "scheme-pad";
    physical.setPad(2, pad);
    f.back();
    f.menu.menu().focus(2);
    f.select();
    f.menu.update(readPlayerMenuInput(physical, f.config, 2), 1);
    const auto& padItems = f.menu.menu().definition().items;
    CHECK(std::ranges::find(padItems, 106, &MenuItem::code) == padItems.end());
}
