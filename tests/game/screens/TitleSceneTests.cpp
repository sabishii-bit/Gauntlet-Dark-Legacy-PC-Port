#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/menu/MenuInput.h"
#include "game/screens/GameContext.h"
#include "game/screens/TitleScene.h"

namespace {

using namespace gdl;
using namespace gdl::game;

MenuInput press(bool start, bool select = false, bool down = false, bool back = false) {
    MenuInput input;
    input.start = start;
    input.select = select;
    input.down = down;
    input.back = back;
    return input;
}

std::filesystem::path unpackedRoot() {
    return test::assetOrSkip("TITLE/textures.ngc").parent_path().parent_path();
}

struct Fixture {
    GameConfig config;
    StringTable strings;

    Fixture() { strings.load(test::dataDirectory() / "text", config.text.language); }

    GameContext context(SoundPlayer* sounds, std::filesystem::path root = unpackedRoot()) const {
        GameContext out;
        out.config = &config;
        out.strings = &strings;
        out.sounds = sounds;
        out.unpackedRoot = std::move(root);
        return out;
    }
};

TEST_CASE("title mouse click opens options and right click leaves them",
          "[game][title][mouse][assets]") {
    test::FakeRenderDevice device;
    const Fixture f;
    TitleScene scene;
    const auto context = f.context(nullptr); // Resolve SKIP before entering an assertion.
    REQUIRE(scene.open(device, context));
    const auto projection = makeLetterboxProjection(640, 448, 1920, 1080);
    scene.render(device, projection, 640, 448);
    MenuInput click;
    click.pointer = Vec2{0.5f, 0.5f};
    click.pointerNormalized = true;
    click.pointerPressed = true;
    scene.step(1, click);
    REQUIRE(scene.menuOpen());
    CHECK_FALSE(scene.optionsOpen());
    // Start / Options occupy two rows, with Options at y=336.
    const auto transform = makeVirtualScreenTransform(projection, 512, 384, 640, 448);
    const auto clip = transform * Vec4{256, 344, 0.5f, 1};
    click.pointer = (Vec2{clip} + Vec2{1}) / 2.0f;
    scene.step(1, click);
    REQUIRE(scene.optionsOpen());
    CHECK(scene.settings().page() == SettingsMenu::Page::Root);
    MenuInput back;
    back.pointerBack = true;
    scene.step(1, back);
    CHECK(scene.settings().menu().closing());
}

TEST_CASE("the title screen refuses to open without unpacked data", "[game][title]") {
    test::FakeRenderDevice device;
    const Fixture f;
    TitleScene scene;
    REQUIRE_FALSE(scene.open(device, f.context(nullptr, test::scratchDirectory("title-empty"))));
    REQUIRE_FALSE(scene.isOpen());
    REQUIRE(scene.step(1, press(true)) == TitleOutcome::Running);
}

TEST_CASE("the title menu renders and plays music directly from the retail tree",
          "[game][title][assets][native-assets]") {
    const auto root = test::assetOrSkip("TITLE/objects.ngc").parent_path().parent_path();
    REQUIRE_FALSE(std::filesystem::exists(root / "TITLE/textures.json"));
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const Fixture fixture;
    TitleScene scene;
    REQUIRE(scene.open(device, fixture.context(&player, root)));
    REQUIRE(scene.arrowBound());
    REQUIRE(scene.musicPlaying());
    REQUIRE(player.voiceCount() == 1);
    scene.step(60, MenuInput{});
    scene.render(device, makeScreenProjection(640.0f, 448.0f), 640.0f, 448.0f);
    REQUIRE(device.draws.size() >= 6);
    REQUIRE(scene.step(1, press(true)) == TitleOutcome::Running);
    REQUIRE(scene.menuOpen());
    REQUIRE(scene.step(1, press(false, true)) == TitleOutcome::Running);
    REQUIRE(scene.startMenuOpen());
    REQUIRE_FALSE(scene.loading());
    REQUIRE(scene.step(1, press(false, true)) == TitleOutcome::Running);
    REQUIRE(scene.loading());
    REQUIRE(scene.step(TitleScene::kLoadingTicks, MenuInput{}) == TitleOutcome::StartGame);
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("the glow fades in and the screen times out when idle", "[game][title][assets]") {
    test::FakeRenderDevice device;
    const Fixture f;
    TitleScene scene;
    const auto context = f.context(nullptr);
    REQUIRE(scene.open(device, context));
    REQUIRE(scene.isOpen());
    REQUIRE(scene.arrowBound());
    REQUIRE(scene.step(1, MenuInput{}) == TitleOutcome::Running);
    REQUIRE(scene.glowOpacity() == 4);
    REQUIRE(scene.step(59, MenuInput{}) == TitleOutcome::Running);
    REQUIRE(scene.glowOpacity() == 255);
    REQUIRE(scene.idleTicks() == TitleScene::kIdleTicks - 60);

    TitleOutcome outcome = TitleOutcome::Running;
    for (s32 i = 0; i < TitleScene::kIdleTicks && outcome == TitleOutcome::Running; ++i) {
        outcome = scene.step(1, MenuInput{});
    }
    REQUIRE(outcome == TitleOutcome::TimedOut);
    REQUIRE(scene.glowOpacity() == 0);
}

TEST_CASE("Start opens Local and Netplay and Local leads into story mode",
          "[game][title][assets]") {
    test::FakeRenderDevice device;
    const Fixture f;
    TitleScene scene;
    const auto context = f.context(nullptr);
    REQUIRE(scene.open(device, context));
    scene.step(100, MenuInput{});
    REQUIRE_FALSE(scene.menuOpen());
    REQUIRE(scene.step(1, press(true)) == TitleOutcome::Running);
    REQUIRE(scene.menuOpen());
    REQUIRE(scene.idleTicks() == TitleScene::kIdleTicks);

    REQUIRE(scene.step(1, press(false, false, false, true)) == TitleOutcome::Running);
    REQUIRE_FALSE(scene.menuOpen());

    scene.step(1, press(true));
    REQUIRE(scene.step(1, press(false, true)) == TitleOutcome::Running);
    REQUIRE(scene.startMenuOpen());
    REQUIRE_FALSE(scene.loading());
    scene.step(1, press(false, false, false, true));
    REQUIRE(scene.menuOpen());
    REQUIRE_FALSE(scene.startMenuOpen());
    scene.step(1, press(false, true));
    REQUIRE(scene.startMenuOpen());
    REQUIRE(scene.step(1, press(false, true)) == TitleOutcome::Running);
    REQUIRE(scene.loading());
    REQUIRE_FALSE(scene.menuOpen());
    TitleOutcome outcome = TitleOutcome::Running;
    s32 ticks = 0;
    while (outcome == TitleOutcome::Running && ticks < 100) {
        outcome = scene.step(1, MenuInput{});
        ++ticks;
    }
    REQUIRE(outcome == TitleOutcome::StartGame);
    REQUIRE(ticks == TitleScene::kLoadingTicks);
}

TEST_CASE("the options menu opens over the title menu and fades away", "[game][title][assets]") {
    test::FakeRenderDevice device;
    const Fixture f;
    TitleScene scene;
    const auto context = f.context(nullptr);
    REQUIRE(scene.open(device, context));
    scene.step(1, press(true));
    scene.step(1, press(false, false, true));
    scene.step(1, press(false, true));
    REQUIRE(scene.optionsOpen());
    REQUIRE(scene.menuOpen());
    CHECK(scene.settings().menu().definition().parchmentFont);
    CHECK_FALSE(scene.settings().menu().definition().prompts);
    CHECK(scene.settings().menu().definition().backLabel.empty());
    CHECK(scene.settings().menu().definition().selectLabel.empty());
    scene.step(1, press(false, false, false, true));
    REQUIRE(scene.optionsOpen());
    scene.step(OptionMenu::kFadeTicks, MenuInput{});
    REQUIRE_FALSE(scene.optionsOpen());
    REQUIRE(scene.menuOpen());
}

TEST_CASE("rendering draws the backdrop, glow and text", "[game][title][assets]") {
    test::FakeRenderDevice device;
    const Fixture f;
    TitleScene scene;
    auto context = f.context(nullptr);
    bool prompted = false;
    context.controlLabels = [&](s32 player, std::string_view action) {
        CHECK(player == -1);
        CHECK(action == "menuStart");
        prompted = true;
        return "F2";
    };
    REQUIRE(scene.open(device, context));
    scene.step(30, MenuInput{});
    const Mat4 projection = makeScreenProjection(640.0f, 448.0f);
    scene.render(device, projection, 640.0f, 448.0f);
    CHECK_FALSE(prompted);
    // The user-requested title prompt is literal, independent of controller remapping.
    CHECK(f.strings.get("title.pressStart") == "Press Start");
    REQUIRE(device.draws.back().vertices.size() == 60U);     // ten non-space glyph quads
    CHECK(test::minCorner(device.draws.back()).y == 320.5f); // glyph raster's half-pixel inset
    REQUIRE(device.draws.size() >= 6);
    REQUIRE(device.draws[0].vertices.size() == 6);
    REQUIRE(device.draws[0].transform == Mat4{1});
    REQUIRE(test::minCorner(device.draws[0]) == Vec2{-1, -1});
    REQUIRE(test::maxCorner(device.draws[0]) == Vec2{1, 1});
    REQUIRE(test::maxCorner(device.draws[1]).x == 256.0f);

    device.draws.clear();
    scene.step(1, press(true));
    scene.render(device, projection, 640.0f, 448.0f);
    REQUIRE(device.draws.size() >= 6);
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("the title screen plays its music and menu sounds", "[game][title][assets]") {
    test::assetOrSkip("audio/SELECT.vbk");
    test::assetOrSkip("audio/COMMON.vbk");
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const Fixture f;
    TitleScene scene;
    const auto context = f.context(&player);
    REQUIRE(scene.open(device, context));
    REQUIRE(scene.musicPlaying());
    REQUIRE(player.voiceCount() == 1);
    // AudioSelect(1), GUNE5D 800a0f64: always SELECT's 0xc0000, not a boss flag.
    SoundSet bank;
    REQUIRE(bank.load(context.unpackedRoot / "audio/SELECT"));
    const auto cue = bank.find("S_SELECTMUS");
    REQUIRE(cue.has_value());
    REQUIRE(bank.entry(*cue).id == 0xc0000);
    REQUIRE(bank.sequence(*cue).loops());
    AudioMixer expectedMixer(48000);
    SoundPlayer expectedPlayer(expectedMixer);
    expectedPlayer.play(bank.sequence(*cue), 1, SoundCategory::Music);
    std::vector<f32> actual(9600);
    std::vector<f32> expected(9600);
    mixer.mix(actual);
    expectedMixer.mix(expected);
    CHECK(actual == expected);
    scene.step(1, press(true));
    REQUIRE(player.voiceCount() == 2);
    scene.step(1, press(false, false, true));
    REQUIRE(player.voiceCount() == 3);
    scene.step(1, press(false, true));
    REQUIRE(scene.optionsOpen());
    REQUIRE(player.voiceCount() == 4);
    scene.step(1, press(false, false, false, true));
    REQUIRE(scene.burning());
    REQUIRE(player.voiceCount() == 5);
    scene.close();
    player.update();
    REQUIRE_FALSE(scene.musicPlaying());
}

TEST_CASE("backing out of the options burns the scroll and blanks the controls",
          "[game][title][assets]") {
    test::FakeRenderDevice device;
    const Fixture f;
    TitleScene scene;
    const auto context = f.context(nullptr);
    REQUIRE(scene.open(device, context));
    scene.step(1, press(true));
    scene.step(1, press(false, false, true));
    scene.step(1, press(false, true));
    REQUIRE(scene.optionsOpen());
    REQUIRE_FALSE(scene.burning());
    device.draws.clear();
    device.textureUpdates = 0;

    scene.step(1, press(false, false, false, true));
    REQUIRE(scene.burning());
    REQUIRE(scene.optionsOpen());
    const Mat4 projection = makeScreenProjection(640.0f, 448.0f);
    scene.render(device, projection, 640.0f, 448.0f);
    REQUIRE(device.textureUpdates == 1);
    REQUIRE_FALSE(device.draws.empty());

    // Input is ignored until the burn ends: Back would otherwise close the title menu.
    scene.step(OptionMenu::kFadeTicks, press(false, false, false, true));
    REQUIRE_FALSE(scene.optionsOpen());
    REQUIRE(scene.burning());
    REQUIRE(scene.menuOpen());
    scene.step(BurnDialogueScroll::kFrameCount * BurnDialogueScroll::kTicksPerFrame,
               press(false, false, false, true));
    REQUIRE_FALSE(scene.burning());
    REQUIRE(scene.menuOpen());
    scene.step(1, press(false, false, false, true));
    REQUIRE_FALSE(scene.menuOpen());
}

TEST_CASE("the clock and screen come from the configuration", "[game][title][assets]") {
    test::FakeRenderDevice device;
    Fixture f;
    f.config.timing.tickRate = 120;
    f.config.display.virtualWidth = 1024;
    TitleScene scene;
    const auto context = f.context(nullptr);
    REQUIRE(scene.open(device, context));
    REQUIRE(scene.tickRate() == 120);
    REQUIRE(scene.screen().width == 1024);
    REQUIRE(scene.update(0.5, MenuInput{}) == TitleOutcome::Running);
    REQUIRE(scene.time() == 6); // capped at six ticks per frame
    REQUIRE(scene.update(1.0 / 120.0 + 1e-6, MenuInput{}) == TitleOutcome::Running);
    REQUIRE(scene.time() == 7);
}

TEST_CASE("title options persist edits without beginning a game",
          "[game][title][settings][assets]") {
    test::FakeRenderDevice device;
    Fixture f;
    auto context = f.context(nullptr);
    const auto file = test::scratchDirectory("title-settings") / "settings.json";
    context.saveSettings = [&](const GameConfig& next) {
        next.saveFile(file);
        f.config = next;
        return true;
    };
    TitleScene scene;
    REQUIRE(scene.open(device, context));
    scene.step(1, press(true));
    scene.step(1, press(false, false, true));
    scene.step(1, press(false, true)); // root options
    scene.step(1, press(false, true)); // audio
    REQUIRE(scene.settings().page() == SettingsMenu::Page::Audio);
    MenuInput left;
    left.left = true;
    CHECK(scene.step(1, left) == TitleOutcome::Running);
    scene.step(1, {}); // release persists the slider's preview
    GameConfig saved;
    REQUIRE(saved.loadFile(file));
    CHECK(AudioSlider::value(saved.audio.musicVolume) == 127);
    CHECK_FALSE(scene.loading());
    scene.step(1, press(false, false, false, true));
    CHECK(scene.settings().page() == SettingsMenu::Page::Root);
    CHECK_FALSE(scene.burning());
}

TEST_CASE("the title offers netplay without beginning offline character selection",
          "[game][title][assets]") {
    test::FakeRenderDevice device;
    const Fixture fixture;
    const auto context = fixture.context(nullptr);
    TitleScene scene;
    REQUIRE(scene.open(device, context));
    scene.step(1, press(true));
    scene.step(1, press(false, true));
    REQUIRE(scene.startMenuOpen());
    scene.step(1, press(false, false, true));
    CHECK(scene.step(1, press(false, true)) == TitleOutcome::Netplay);
    CHECK_FALSE(scene.loading());
}

TEST_CASE("mouse follows Start to Local or Netplay and back preserves title navigation",
          "[game][title][mouse][assets]") {
    test::FakeRenderDevice device;
    const Fixture fixture;
    const auto context = fixture.context(nullptr);
    TitleScene scene;
    REQUIRE(scene.open(device, context));
    scene.render(device, makeScreenProjection(512, 384), 512, 384);
    MenuInput click;
    click.pointer = Vec2{256, 312};
    click.pointerPressed = true;
    scene.step(1, click); // Press Start
    REQUIRE(scene.menuOpen());
    CHECK_FALSE(scene.startMenuOpen());
    scene.step(1, click); // Start
    REQUIRE(scene.startMenuOpen());
    CHECK_FALSE(scene.loading());
    MenuInput back;
    back.pointerBack = true;
    scene.step(1, back);
    REQUIRE(scene.menuOpen());
    CHECK_FALSE(scene.startMenuOpen());
    scene.step(1, click);
    REQUIRE(scene.startMenuOpen());
    click.pointer = Vec2{256, 344};
    CHECK(scene.step(1, click) == TitleOutcome::Netplay);
    CHECK_FALSE(scene.loading());
    // Leaving the netplay overlay returns to this page, not offline character selection.
    scene.step(1, back);
    CHECK_FALSE(scene.startMenuOpen());
    scene.step(1, back);
    CHECK_FALSE(scene.menuOpen());
}
} // namespace
