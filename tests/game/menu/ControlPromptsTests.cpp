#include <catch2/catch_test_macros.hpp>

#include "game/menu/ControlPrompts.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("control prompts resolve the actual per-player primary binding", "[controls][prompts]") {
    GameConfig config;
    CHECK(boundControlLabel(config, 0, true, "attack") == "LMB");
    CHECK(boundControlLabel(config, 0, true, "menuSelect") == "Enter");
    CHECK(boundControlLabel(config, 0, false, "menuSelect") == "A");
    config.menu.padSelect = {PadButton::Y};
    CHECK(boundControlLabel(config, 1, false, "menuSelect") == "Y");
    auto& profile = config.controls[1];
    profile.customized = true;
    profile.menu.padSelect = {PadButton::RightBumper, PadButton::A};
    profile.menu.select = {Key::F2};
    CHECK(boundControlLabel(config, 1, false, "menuSelect") == "RB");
    CHECK(boundControlLabel(config, 1, true, "menuSelect") == "F2");
    CHECK(boundControlLabel(config, 0, false, "menuSelect") == "Y");
    profile.menu.padSelect.clear();
    CHECK(boundControlLabel(config, 1, false, "menuSelect") == "Unbound");
    profile.device = "none";
    CHECK(boundControlLabel(config, 1, true, "menuSelect") == "Unbound");
    CHECK(boundControlLabel(config, -1, true, "attack") == "Unbound");
    CHECK(boundControlLabel(config, 4, true, "attack") == "Unbound");
    CHECK(boundControlLabel(config, 0, true, "unknown") == "Unbound");
}

TEST_CASE("potion prompts preserve hold and double-tap gestures unless directly bound",
          "[controls][prompts]") {
    GameConfig config;
    config.play.padUsePotion = {PadButton::Y};
    config.play.padThrowPotion.clear();
    config.play.padShieldPotion.clear();
    config.play.padMagicGestures = true;
    CHECK(boundControlLabel(config, 0, false, "throwPotion") == "Y (hold)");
    CHECK(boundControlLabel(config, 0, false, "shieldPotion") == "Y (double tap)");
    config.play.padThrowPotion = {PadButton::RightTrigger};
    CHECK(boundControlLabel(config, 0, false, "throwPotion") == "RT");
    config.play.padMagicGestures = false;
    CHECK(boundControlLabel(config, 0, false, "shieldPotion") == "Unbound");
}

TEST_CASE("action tokens do not rewrite ordinary letters or action names", "[controls][prompts]") {
    s32 owner = -1;
    const ControlLabels labels = [&](s32 player, std::string_view action) {
        owner = player;
        return action == "menuStart" ? "F2" : "RMB";
    };
    CHECK(controlText("Press {bind:menuStart}, then {bind:menuSelect}", labels, 2) ==
          "Press F2, then RMB");
    CHECK(owner == 2);
    CHECK(controlText("A key, B: buy, START, X-Ray", labels, 1) == "A key, B: buy, START, X-Ray");
    CHECK(controlText("Press {bind:unfinished", labels, 1) == "Press {bind:unfinished");
}

TEST_CASE("prompt device follows input and keeps multiplayer bindings separate",
          "[controls][prompts]") {
    GameConfig config;
    Input input;
    PadSnapshot pad;
    pad.connected = true;
    pad.guid = "test";
    input.setPad(0, pad);
    input.beginPoll();
    PromptDevices prompts;
    prompts.update(input, config);
    CHECK(prompts.label(input, config, 0, "menuSelect") == "Enter");
    pad.buttons[static_cast<usize>(PadButton::A)] = true;
    input.setPad(0, pad);
    prompts.update(input, config);
    CHECK(prompts.label(input, config, 0, "menuSelect") == "A");
    input.beginPoll();
    input.setKey(Key::W, true);
    prompts.update(input, config);
    CHECK(prompts.label(input, config, 0, "menuSelect") == "Enter");
    config.controls[2].device = "keyboard";
    config.controls[2].customized = true;
    config.controls[2].menu.select = {Key::F3};
    input.beginPoll();
    input.setKey(Key::F3, true);
    prompts.update(input, config);
    CHECK(prompts.lastPlayer() == 2);
    CHECK(prompts.label(input, config, 2, "menuSelect") == "F3");
    CHECK(prompts.label(input, config, 0, "menuSelect") == "A");
    config.controls[0].device = "test";
    input.setPad(0, {});
    CHECK(prompts.label(input, config, 0, "menuSelect") == "A");
}
} // namespace
