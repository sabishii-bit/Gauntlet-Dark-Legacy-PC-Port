#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/Error.h"

#include "game/config/ControlProfiles.h"
#include "game/menu/MenuInput.h"
#include "game/players/PlayerControls.h"

namespace {
using namespace gdl;
using namespace gdl::game;
PadSnapshot pad(std::string guid) {
    PadSnapshot result;
    result.connected = true;
    result.guid = std::move(guid);
    result.name = "Test controller";
    return result;
}
TEST_CASE("player profiles persist independent mouse and controller mappings",
          "[controls][config]") {
    GameConfig config;
    config.controls[2].device = "keyboard";
    config.controls[2].customized = true;
    config.controls[2].play.attack = {Key::MouseRight};
    config.controls[2].menu.start = {Key::F2};
    config.controls[1].device = "test-guid";
    config.controls[1].occurrence = 1;
    config.controls[1].customized = true;
    config.controls[1].play.padAttack = {PadButton::Button12};
    GameConfig restored;
    restored.mergeJson(config.toJson());
    CHECK(restored.toJson() == config.toJson());
    CHECK(playBindings(restored, 2).attack == std::vector{Key::MouseRight});
    CHECK(playBindings(restored, 0).attack == config.play.attack);
    CHECK(menuBindings(restored, 2).start == std::vector{Key::F2});
    Input input;
    input.beginPoll();
    input.setKey(Key::F2, true);
    CHECK(readSharedMenuInput(input, restored).start);
    input.beginPoll();
    input.setKey(Key::F2, false);
    input.setKey(Key::Enter, true);
    CHECK_FALSE(readSharedMenuInput(input, restored).start);
    auto invalid = nlohmann::json::parse(config.toJson());
    invalid["controls"]["players"][1]["occurrence"] = -1;
    CHECK_THROWS_AS(restored.mergeJson(invalid.dump()), FormatError);
}
TEST_CASE("explicit devices reserve physical slots without stealing another player's keyboard",
          "[controls][multiplayer]") {
    GameConfig config;
    Input input;
    input.setPad(0, pad("alpha"));
    input.setPad(7, pad("beta"));
    config.controls[3].device = "alpha";
    config.controls[2].device = "keyboard";
    config.controls[1].device = "beta";
    auto devices = controlDevices(config, input);
    CHECK_FALSE(devices[0].keyboard);
    CHECK(devices[0].pad == -2);
    CHECK(devices[1].pad == 7);
    CHECK(devices[2].keyboard);
    CHECK(devices[2].pad == -2);
    CHECK(devices[3].pad == 0);
    input.setKey(Key::W, true);
    CHECK_FALSE(readPlayerMenuInput(input, config, 0).up);
    CHECK(readPlayerMenuInput(input, config, 2).up);
    input.setPad(7, {});
    CHECK(controlDevices(config, input)[1].pad == -2);
}
TEST_CASE("unplugging one identical controller does not reassign the other",
          "[controls][multiplayer]") {
    GameConfig config;
    Input input;
    input.setPad(0, pad("same"));
    input.setPad(1, pad("same"));
    config.controls[0].device = "same";
    config.controls[1].device = "same";
    config.controls[1].occurrence = 1;
    REQUIRE(controlDevices(config, input)[1].pad == 1);
    input.setPad(0, {});
    const auto devices = controlDevices(config, input);
    CHECK(devices[0].pad == -2);
    CHECK(devices[1].pad == 1);
}
TEST_CASE("a different device reusing a slot cannot inherit buffered button presses",
          "[controls][input]") {
    Input polled;
    Input buffered;
    auto old = pad("old");
    old.buttons[static_cast<usize>(PadButton::A)] = true;
    polled.beginPoll();
    polled.setPad(0, old);
    buffered.accumulate(polled);
    REQUIRE(buffered.wasPadButtonPressed(0, PadButton::A));
    polled.beginPoll();
    polled.setPad(0, pad("new"));
    buffered.accumulate(polled);
    CHECK_FALSE(buffered.wasPadButtonPressed(0, PadButton::A));
    CHECK_FALSE(buffered.isPadButtonDown(0, PadButton::A));
}

TEST_CASE("remapped player actions retain same-device turbo and potion chords",
          "[controls][multiplayer]") {
    GameConfig config;
    config.controls[3].customized = true;
    config.controls[3].device = "pad";
    config.controls[3].play.padAttack = {PadButton::Y};
    config.controls[3].play.padTurbo = {PadButton::LeftBumper};
    Input input;
    auto state = pad("pad");
    state.buttons[static_cast<usize>(PadButton::Y)] = true;
    state.buttons[static_cast<usize>(PadButton::LeftBumper)] = true;
    input.beginPoll();
    input.setPad(8, state);
    const auto source = playerInputSource(input, config, 3);
    const auto buttons =
        readPlayButtons(input, playBindings(config, 3), source.keyboard, source.pad);
    CHECK(buttons.turboAttackPressed);
    CHECK_FALSE(readPlayButtons(input, playBindings(config, 0), true, 0).turboAttackPressed);
}
TEST_CASE("pause navigation recovers from a disabled or disconnected owner without stealing input",
          "[controls][menu][pause]") {
    GameConfig config;
    Input input;
    input.setKey(Key::Enter, true);
    auto controller = pad("controller");
    input.setPad(1, controller);
    CHECK_FALSE(readPauseMenuInput(input, config, 1).select);
    config.controls[1].device = "none";
    CHECK(readPauseMenuInput(input, config, 1).select);
    config.controls[1].device = "controller";
    CHECK_FALSE(readPauseMenuInput(input, config, 1).select);
    input.setPad(1, {});
    CHECK(readPauseMenuInput(input, config, 1).select);
}

TEST_CASE("legacy implicit movement imports as explicit stick bindings without changing new remaps",
          "[controls][config]") {
    GameConfig config;
    auto legacy = nlohmann::json::parse(config.toJson());
    legacy["controls"].erase("explicitMovement");
    for (const auto* direction : {"up", "down", "left", "right"}) {
        legacy["controls"]["play"]["pad"][direction] = nlohmann::json::array();
    }
    legacy["controls"]["players"][1]["bindings"]["up"]["buttons"] = {"DpadUp"};
    config.mergeJson(legacy.dump());
    CHECK(config.play.padUp == std::vector{PadButton::LeftStickUp});
    CHECK(config.controls[1].play.padUp == std::vector{PadButton::DpadUp, PadButton::LeftStickUp});
    config.controls[1].play.padUp = {PadButton::DpadDown};
    config.controls[1].play.padDown.clear();
    GameConfig restored;
    restored.mergeJson(config.toJson());
    CHECK(restored.controls[1].play.padUp == std::vector{PadButton::DpadDown});
    CHECK(restored.controls[1].play.padDown.empty());
    CHECK(restored.toJson() == config.toJson());
}
} // namespace
