#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/MenuInput.h"
#include "game/TitleScene.h"

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

TEST_CASE("the title screen refuses to open without unpacked data", "[game][title]") {
    test::FakeRenderDevice device;
    TitleScene scene;
    REQUIRE_FALSE(scene.open(device, nullptr, test::scratchDirectory("title-empty")));
    REQUIRE_FALSE(scene.isOpen());
    REQUIRE(scene.step(1, press(true)) == TitleOutcome::Running);
}

TEST_CASE("the glow fades in and the screen times out when idle", "[game][title][unpacked]") {
    test::FakeRenderDevice device;
    TitleScene scene;
    REQUIRE(scene.open(device, nullptr, unpackedRoot()));
    REQUIRE(scene.isOpen());
    REQUIRE(scene.arrowBound());
    REQUIRE(scene.step(1, MenuInput{}) == TitleOutcome::Running);
    REQUIRE(scene.glowOpacity() == 4);
    REQUIRE(scene.step(59, MenuInput{}) == TitleOutcome::Running);
    REQUIRE(scene.glowOpacity() == 255);
    REQUIRE(scene.idleTicks() == TitleScene::kIdleTicks - 60);

    TitleOutcome outcome = TitleOutcome::Running;
    for (int i = 0; i < TitleScene::kIdleTicks && outcome == TitleOutcome::Running; ++i) {
        outcome = scene.step(1, MenuInput{});
    }
    REQUIRE(outcome == TitleOutcome::TimedOut);
    REQUIRE(scene.glowOpacity() == 0);
}

TEST_CASE("start opens the menu and choosing start leads into the game",
          "[game][title][unpacked]") {
    test::FakeRenderDevice device;
    TitleScene scene;
    REQUIRE(scene.open(device, nullptr, unpackedRoot()));
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
    int ticks = 0;
    while (outcome == TitleOutcome::Running && ticks < 100) {
        outcome = scene.step(1, MenuInput{});
        ++ticks;
    }
    REQUIRE(outcome == TitleOutcome::StartGame);
    REQUIRE(ticks == TitleScene::kLoadingTicks);
}

TEST_CASE("the options menu opens over the title menu and fades away", "[game][title][unpacked]") {
    test::FakeRenderDevice device;
    TitleScene scene;
    REQUIRE(scene.open(device, nullptr, unpackedRoot()));
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
    TitleScene scene;
    REQUIRE(scene.open(device, nullptr, unpackedRoot()));
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
    TitleScene scene;
    REQUIRE(scene.open(device, &player, unpackedRoot()));
    REQUIRE(scene.musicPlaying());
    REQUIRE(player.voiceCount() == 1);
    scene.step(1, press(true));
    REQUIRE(player.voiceCount() == 2);
    scene.step(1, press(false, false, true));
    REQUIRE(player.voiceCount() == 3);
    scene.close();
    player.update();
    REQUIRE_FALSE(scene.musicPlaying());
}

} // namespace
