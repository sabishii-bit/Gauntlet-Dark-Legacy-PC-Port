#include <filesystem>

#include <catch2/catch_test_macros.hpp>

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
    return test::unpackedOrSkip("TITLE/textures.json").parent_path().parent_path();
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

TEST_CASE("the title screen refuses to open without unpacked data", "[game][title]") {
    test::FakeRenderDevice device;
    const Fixture f;
    TitleScene scene;
    REQUIRE_FALSE(scene.open(device, f.context(nullptr, test::scratchDirectory("title-empty"))));
    REQUIRE_FALSE(scene.isOpen());
    REQUIRE(scene.step(1, press(true)) == TitleOutcome::Running);
}

TEST_CASE("the glow fades in and the screen times out when idle", "[game][title][unpacked]") {
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

TEST_CASE("start opens the menu and choosing start leads into the game",
          "[game][title][unpacked]") {
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

TEST_CASE("the options menu opens over the title menu and fades away", "[game][title][unpacked]") {
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
    scene.step(1, press(false, false, false, true));
    REQUIRE(scene.optionsOpen());
    scene.step(OptionMenu::kFadeTicks, MenuInput{});
    REQUIRE_FALSE(scene.optionsOpen());
    REQUIRE(scene.menuOpen());
}

TEST_CASE("rendering draws the backdrop, glow and text", "[game][title][unpacked]") {
    test::FakeRenderDevice device;
    const Fixture f;
    TitleScene scene;
    const auto context = f.context(nullptr);
    REQUIRE(scene.open(device, context));
    scene.step(30, MenuInput{});
    const Mat4 projection = makeScreenProjection(640.0f, 448.0f);
    scene.render(device, projection, 640.0f, 448.0f);
    REQUIRE(device.draws.size() >= 6);
    REQUIRE(device.draws[0].vertices.size() == 6);
    REQUIRE(test::maxCorner(device.draws[0]).x == 256.0f);

    device.draws.clear();
    scene.step(1, press(true));
    scene.render(device, projection, 640.0f, 448.0f);
    REQUIRE(device.draws.size() >= 6);
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("the title screen plays its music and menu sounds", "[game][title][unpacked]") {
    test::unpackedOrSkip("audio/SELECT/sounds.json");
    test::unpackedOrSkip("audio/COMMON/sounds.json");
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const Fixture f;
    TitleScene scene;
    const auto context = f.context(&player);
    REQUIRE(scene.open(device, context));
    REQUIRE(scene.musicPlaying());
    REQUIRE(player.voiceCount() == 1);
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
          "[game][title][unpacked]") {
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

TEST_CASE("the clock and screen come from the configuration", "[game][title][unpacked]") {
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
          "[game][title][settings][unpacked]") {
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

} // namespace
