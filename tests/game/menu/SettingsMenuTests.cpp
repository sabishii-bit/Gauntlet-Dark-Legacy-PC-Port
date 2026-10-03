#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
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
    f.menu.open(f.config, &f.strings, {}, f.painter, {}, MenuDefinition::parchment());
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
    CHECK(open(SettingsMenu::Scope::Title) == std::vector<s32>{0, 1, 2, 4, 3});
    CHECK(open(SettingsMenu::Scope::Tower) == std::vector<s32>{0, 2, 4, 3});
    CHECK(open(SettingsMenu::Scope::Level) == std::vector<s32>{0, 4, 3});
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
    REQUIRE(f.menu.menu().definition().items.size() == 11);
    CHECK(f.menu.menu().definition().items[0].value == "On");
    CHECK(f.menu.menu().definition().items[1].value == "30");
    CHECK(f.menu.menu().definition().items[2].value == "Off");
    f.fail = true;
    f.select();
    CHECK(f.config.display.vsync);
    CHECK(f.menu.config().display.vsync);
    CHECK(f.writes == 0);
    f.fail = false;
    f.right();
    CHECK_FALSE(f.menu.config().display.vsync);
    CHECK(f.config.display.vsync);
    CHECK(f.writes == 0);
    f.down();
    for (const u32 rate : {60U, 0U, 30U, 60U, 0U, 30U}) {
        f.right();
        CHECK(f.menu.config().display.maxFrameRate == rate);
        CHECK(f.menu.config().timing.gameplayFrameRate == rate);
        CHECK(f.menu.menu().definition().items[1].value ==
              (rate == 0 ? "Unlimited" : std::to_string(rate)));
    }
    f.fail = true;
    f.select();
    CHECK(f.config.display.maxFrameRate == 30);
    CHECK(f.menu.config().timing.gameplayFrameRate == 30);
    f.fail = false;
    MenuInput left;
    left.left = true;
    f.menu.update(left, 1);
    CHECK(f.menu.config().display.maxFrameRate == 0);
    f.down();
    for (const u32 samples : {2U, 4U, 1U}) {
        f.right();
        CHECK(f.menu.config().display.sampleCount == samples);
    }
    f.fail = true;
    f.select();
    CHECK(f.config.display.sampleCount == 1);
    CHECK(f.menu.config().display.sampleCount == 1);
    f.back();
    CHECK(f.menu.page() == SettingsMenu::Page::Root);
    CHECK(f.menu.menu().selection() == 3);
    CHECK(f.menu.config().display.vsync);
    CHECK(f.writes == 0);
}

TEST_CASE("Video chooses supported resolutions and borderless uses the desktop without losing the "
          "preference",
          "[settings][graphics]") {
    Fixture f;
    f.menu.open(f.config, &f.strings,
                [&](const auto& next) {
                    if (f.fail) {
                        return false;
                    }
                    f.config = next;
                    return true;
                },
                f.painter, {}, {}, SettingsMenu::Scope::Level, {},
                {{1920, 1080}, {{1280, 720}, {1920, 1080}}});
    f.down();
    f.select();
    REQUIRE(f.menu.menu().definition().title == "Video");
    f.down();
    f.down();
    f.down();
    REQUIRE(f.menu.menu().selection() == 4); // Windowed resolution is skipped.
    CHECK_FALSE(f.menu.menu().definition().items[3].enabled);
    f.right();
    CHECK(f.menu.config().display.windowMode == WindowMode::Fullscreen);
    MenuInput up;
    up.up = true;
    f.menu.update(up, 1);
    REQUIRE(f.menu.menu().selection() == 3);
    f.right();
    CHECK(f.menu.config().display.windowWidth == 1280);
    CHECK(f.menu.config().display.windowHeight == 720);
    f.down();
    f.right();
    CHECK(f.menu.config().display.windowMode == WindowMode::BorderlessFullscreen);
    CHECK(f.menu.menu().definition().items[3].value == "1920 x 1080");
    CHECK_FALSE(f.menu.menu().definition().items[3].enabled);
    CHECK(f.menu.config().display.windowWidth == 1280);
    f.right();
    CHECK(f.menu.config().display.windowMode == WindowMode::Windowed);
    CHECK(f.menu.menu().definition().items[3].value == "1280 x 720");
    CHECK_FALSE(f.menu.menu().definition().items[3].enabled);
}

TEST_CASE("fullscreen replaces a custom window size with an advertised desktop mode",
          "[settings][graphics]") {
    Fixture f;
    f.menu.open(f.config, &f.strings,
                [&](const auto& next) {
                    f.config = next;
                    return true;
                },
                f.painter, {}, {}, SettingsMenu::Scope::Level, {}, {{1920, 1080}, {{1920, 1080}}});
    f.down();
    f.select();
    f.down();
    f.down();
    f.down();
    REQUIRE(f.menu.menu().selection() == 4);
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
        f.menu.open(f.config, &f.strings, {}, f.painter, {}, MenuDefinition::parchment(), scope);
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
                CHECK(f.menu.menu().itemY(static_cast<s32>(i)) +
                          static_cast<s32>(32 * definition.scale) + 12 <
                      300);
                CHECK(definition.valueX > f.menu.menu().itemX(static_cast<s32>(i)) +
                                              f.painter.measure(item.text, definition.scale));
                CHECK(definition.valueX + f.painter.measure(item.value, definition.scale) + 11 <=
                      448);
            } else {
                CHECK(f.menu.menu().itemY(static_cast<s32>(i)) == 300);
            }
        }
        f.back();
        CHECK(f.menu.page() == SettingsMenu::Page::Root);
        CHECK(f.menu.menu().selection() == selected);
    }
}

TEST_CASE("custom file frame rates are shown honestly until a supported choice is selected",
          "[settings][graphics]") {
    Fixture f;
    f.config.timing.gameplayFrameRate = 24;
    f.menu.open(
        f.config, &f.strings,
        [&](const auto& next) {
            f.config = next;
            return true;
        },
        f.painter, {}, {}, SettingsMenu::Scope::Level);
    f.down();
    f.select();
    CHECK(f.menu.menu().definition().items[1].value == "24");
    f.down();
    f.right();
    CHECK(f.menu.config().timing.gameplayFrameRate == 30);
    CHECK(f.menu.config().display.maxFrameRate == 30);
    CHECK(f.config.timing.gameplayFrameRate == 24);
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

TEST_CASE("Video preview requires confirmation and uses a wall-clock rollback deadline",
          "[settings][graphics]") {
    Fixture f;
    GameConfig active = f.config;
    f64 now = 100;
    s32 previews = 0;
    f.menu.open(
        f.config, &f.strings,
        [&](const auto& next) {
            ++f.writes;
            f.config = next;
            return true;
        },
        f.painter, {}, {}, SettingsMenu::Scope::Level, {}, {}, {},
        [&](const auto& next) {
            ++previews;
            active = next;
            return true;
        },
        [&] { return now; });
    f.down();
    f.select();
    f.right();
    CHECK(active.display.vsync);
    f.down();
    f.down();
    f.down(); // skips unavailable resolution/window mode, reaches Depth of Field
    CHECK(f.menu.menu().definition().items[5].text == "Depth of Field");
    f.right();
    CHECK(f.menu.config().display.depthOfField);
    CHECK_FALSE(active.display.depthOfField);
    f.down();
    REQUIRE(f.menu.menu().selection() == 6);
    CHECK(f.menu.menu().definition().items[6].text == "Bloom");
    f.right();
    CHECK(f.menu.config().display.bloom);
    CHECK_FALSE(active.display.bloom);
    f.down();
    REQUIRE(f.menu.menu().selection() == 7);
    CHECK(f.menu.menu().definition().items[7].text == "Ambient Occlusion");
    f.right();
    CHECK(f.menu.config().display.ambientOcclusion);
    CHECK_FALSE(active.display.ambientOcclusion);
    f.down();
    REQUIRE(f.menu.menu().selection() == 8);
    f.select();
    REQUIRE(f.menu.menu().definition().items.size() == 2);
    CHECK(f.menu.menu().selection() == 1); // default to Revert, not Save
    CHECK_FALSE(active.display.vsync);
    CHECK(active.display.depthOfField);
    CHECK(active.display.bloom);
    CHECK(active.display.ambientOcclusion);
    CHECK(f.config.display.vsync);
    CHECK(f.writes == 0);
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
        CHECK(f.writes == 1);
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
        CHECK(f.writes == 0);
        CHECK(f.menu.menu().definition().items.size() == 11);
        CHECK_FALSE(active.display.depthOfField);
        CHECK_FALSE(active.display.ambientOcclusion);
        CHECK_FALSE(active.display.bloom);
    }
    SECTION("Back cancels the trial") {
        f.back();
        CHECK(active.display.vsync);
        CHECK_FALSE(active.display.bloom);
        CHECK(f.writes == 0);
    }
    SECTION("Closing the owner restores the saved configuration") {
        f.menu.close();
        CHECK(active.display.vsync);
        CHECK_FALSE(active.display.bloom);
        CHECK(f.writes == 0);
    }
}

TEST_CASE("Video defaults stay staged and the action row navigates horizontally",
          "[settings][graphics]") {
    Fixture f;
    f.config.display.vsync = false;
    f.config.display.depthOfField = true;
    f.config.display.bloom = true;
    f.config.display.ambientOcclusion = true;
    f.config.audio.effectsVolume = 0.25f;
    f.menu.open(f.config, &f.strings, {}, f.painter, {}, {}, SettingsMenu::Scope::Level);
    f.down();
    f.select();
    f.down();
    f.down();
    f.down();
    f.down();
    f.down();
    f.down();
    REQUIRE(f.menu.menu().selection() == 8);
    f.right();
    REQUIRE(f.menu.menu().selection() == 9);
    f.select();
    CHECK(f.menu.config().display.vsync);
    CHECK_FALSE(f.menu.config().display.depthOfField);
    CHECK_FALSE(f.menu.config().display.bloom);
    CHECK_FALSE(f.menu.config().display.ambientOcclusion);
    CHECK(f.menu.config().audio.effectsVolume == 0.25f);
    CHECK_FALSE(f.config.display.vsync);
    MenuInput up;
    up.up = true;
    f.menu.update(up, 1);
    CHECK(f.menu.menu().selection() == 7); // last enabled setting: Ambient Occlusion
    f.down();
    f.right();
    f.right();
    REQUIRE(f.menu.menu().selection() == 10);
    f.select();
    CHECK(f.menu.page() == SettingsMenu::Page::Root);
    CHECK_FALSE(f.menu.config().display.vsync);
    CHECK(f.writes == 0);
}

TEST_CASE("Video rollback preserves a manually resized window", "[settings][graphics]") {
    Fixture f;
    GameConfig active = f.config;
    f.menu.open(f.config, &f.strings, {}, f.painter, {}, {}, SettingsMenu::Scope::Level, {},
                {{1920, 1080}, {{1920, 1080}}, {1152, 700}}, {}, [&](const auto& next) {
                    active = next;
                    return true;
                });
    f.down();
    f.select();
    CHECK(f.menu.menu().definition().items[3].value == "1152 x 700");
    f.down();
    f.down();
    f.down();
    REQUIRE(f.menu.menu().selection() == 4);
    f.right();
    f.down();
    f.down();
    f.down();
    f.down(); // past Depth of Field, Bloom and Ambient Occlusion to Apply
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
    f.menu.open(f.config, &f.strings, {}, f.painter, {}, MenuDefinition::parchment(),
                SettingsMenu::Scope::Level, {}, {{1920, 1080}, {{1280, 896}, {1920, 1080}}});
    f.down();
    f.select();
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
    REQUIRE(f.menu.menu().selection() == 4);
    for (s32 i = 0; i < 3; ++i) {
        f.right();
        unchanged();
    }
    f.down();
    f.down();
    f.down();
    f.down(); // past Depth of Field, Bloom and Ambient Occlusion to Apply
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
    f.menu.open(
        f.config, &f.strings,
        [&](const auto& next) {
            f.config = next;
            ++f.writes;
            return true;
        },
        f.painter, {}, {}, SettingsMenu::Scope::Level, {},
        {{1920, 1080}, {{1920, 1080}}, actualSize}, {},
        [&](const auto& next) {
            if (!next.display.matchesWindow(WindowMode::Windowed, actualSize)) {
                actualSize = {next.display.windowWidth, next.display.windowHeight};
                ++resizes;
            }
            return true;
        });
    f.down();
    f.select();
    for (s32 i = 0; i < 7; ++i) {
        f.down();
    }
    REQUIRE(f.menu.menu().selection() == 8);
    f.right();
    f.select();
    CHECK(f.menu.config().display.windowMode == WindowMode::Windowed);
    CHECK(f.menu.config().display.windowWidth == 1280);
    CHECK(f.menu.config().display.windowHeight == 896);
    CHECK(f.menu.config().display.maxFrameRate == 30);
    CHECK(f.menu.config().timing.gameplayFrameRate == 30);
    CHECK(actualSize == Extent2D{1600, 1000});
    CHECK(f.writes == 0);
    MenuInput left;
    left.left = true;
    f.menu.update(left, 1);
    f.select(); // Apply
    CHECK(actualSize == Extent2D{1280, 896});
    CHECK(resizes == 1);
    CHECK(f.writes == 0);
    SECTION("Save keeps defaults") {
        f.menu.update(left, 1);
        f.select();
        CHECK(f.writes == 1);
        CHECK(f.config.display.maxFrameRate == 30);
        CHECK(f.config.timing.gameplayFrameRate == 30);
        CHECK(actualSize == Extent2D{1280, 896});
    }
    SECTION("Revert restores the manually resized window") {
        f.back();
        CHECK(actualSize == Extent2D{1600, 1000});
        CHECK(f.menu.config().timing.gameplayFrameRate == 60);
        CHECK(resizes == 2);
        CHECK(f.writes == 0);
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

TEST_CASE("Controls stays disabled in every scope and cannot mutate bindings", "[settings]") {
    Fixture f;
    for (const auto scope :
         {SettingsMenu::Scope::Title, SettingsMenu::Scope::Tower, SettingsMenu::Scope::Level}) {
        f.menu.open(f.config, &f.strings, {}, f.painter, {}, {}, scope);
        const auto& items = f.menu.menu().definition().items;
        const auto controls = std::ranges::find(items, 3, &MenuItem::code);
        REQUIRE(controls != items.end());
        CHECK_FALSE(controls->enabled);
        const auto bindings = f.config.toJson();
        for (usize i = 0; i < items.size() * 2; ++i) {
            f.down();
            CHECK(items[static_cast<usize>(f.menu.menu().selection())].code != 3);
        }
        CHECK(f.config.toJson() == bindings);
        CHECK(f.writes == 0);
    }
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
    f.fail = true;
    f.select();
    CHECK(f.writes == 0);
    CHECK(f.config.multiplayer.mode == MultiplayerMode::Normal);
    CHECK(f.menu.config().multiplayer.mode == MultiplayerMode::Normal);
    CHECK(f.menu.menu().definition().items[0].markedPart == 1);
    CHECK(f.menu.menu().definition().items[1].markedPart == 0);
    f.fail = false;
    f.select();
    CHECK(f.config.multiplayer.mode == MultiplayerMode::Stun);
    CHECK(f.writes == 1);
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
} // namespace
