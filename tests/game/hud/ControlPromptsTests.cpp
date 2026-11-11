#include <string>
#include <string_view>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/hud/ControlPrompts.h"
#include "game/hud/ScriptedControlLabels.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<ControlLabels>);
static_assert(std::has_virtual_destructor_v<ControlLabels>);
static_assert(std::is_base_of_v<ControlLabels, test::ScriptedControlLabels>);

/** The shipped English table, whose words name an unbound action and the potion gestures. */
StringTable shippedText() {
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    return strings;
}

TEST_CASE("control prompts resolve the actual per-player primary binding", "[controls][prompts]") {
    GameConfig config;
    const StringTable strings = shippedText();
    CHECK(boundControlLabel(config, &strings, 0, true, "attack") == "LMB");
    CHECK(boundControlLabel(config, &strings, 0, true, "menuSelect") == "Enter");
    CHECK(boundControlLabel(config, &strings, 0, false, "menuSelect") == "A");
    config.menu.padSelect = {PadButton::Y};
    CHECK(boundControlLabel(config, &strings, 1, false, "menuSelect") == "Y");
    auto& profile = config.controls[1];
    profile.customized = true;
    profile.menu.padSelect = {PadButton::RightBumper, PadButton::A};
    profile.menu.select = {Key::F2};
    CHECK(boundControlLabel(config, &strings, 1, false, "menuSelect") == "RB");
    CHECK(boundControlLabel(config, &strings, 1, true, "menuSelect") == "F2");
    CHECK(boundControlLabel(config, &strings, 0, false, "menuSelect") == "Y");
    profile.menu.padSelect.clear();
    CHECK(boundControlLabel(config, &strings, 1, false, "menuSelect") == "Unbound");
    profile.device = "none";
    CHECK(boundControlLabel(config, &strings, 1, true, "menuSelect") == "Unbound");
    CHECK(boundControlLabel(config, &strings, -1, true, "attack") == "Unbound");
    CHECK(boundControlLabel(config, &strings, 4, true, "attack") == "Unbound");
    CHECK(boundControlLabel(config, &strings, 0, true, "unknown") == "Unbound");
}

TEST_CASE("potion prompts preserve hold and double-tap gestures unless directly bound",
          "[controls][prompts]") {
    GameConfig config;
    const StringTable strings = shippedText();
    config.play.padUsePotion = {PadButton::Y};
    config.play.padThrowPotion.clear();
    config.play.padShieldPotion.clear();
    config.play.padMagicGestures = true;
    CHECK(boundControlLabel(config, &strings, 0, false, "throwPotion") == "Y (hold)");
    CHECK(boundControlLabel(config, &strings, 0, false, "shieldPotion") == "Y (double tap)");
    config.play.padThrowPotion = {PadButton::RightTrigger};
    CHECK(boundControlLabel(config, &strings, 0, false, "throwPotion") == "RT");
    config.play.padMagicGestures = false;
    CHECK(boundControlLabel(config, &strings, 0, false, "shieldPotion") == "Unbound");
}

TEST_CASE("prompts word an unbound action and the potion gestures in the table's language",
          "[controls][prompts][text]") {
    const auto dir = test::scratchDirectory("prompt-words");
    writeTextFile(dir / "en.json", R"({"controls.unbound": "Libre", "controls.hold": "tenir",
        "controls.doubleTap": "deux fois"})");
    StringTable strings;
    REQUIRE(strings.load(dir, "en"));
    GameConfig config;
    config.play.padUsePotion = {PadButton::Y};
    config.play.padThrowPotion.clear();
    config.play.padShieldPotion.clear();
    config.play.padMagicGestures = true;
    CHECK(boundControlLabel(config, &strings, 0, false, "throwPotion") == "Y (tenir)");
    CHECK(boundControlLabel(config, &strings, 0, false, "shieldPotion") == "Y (deux fois)");
    CHECK(boundControlLabel(config, &strings, 0, true, "unknown") == "Libre");
    // The device's prompt hands its table on.
    const Input input;
    const PromptDevices prompts;
    CHECK(prompts.label(input, config, &strings, 0, "unknown") == "Libre");
    // Without a table each word reads as its text id.
    CHECK(boundControlLabel(config, nullptr, 0, false, "throwPotion") == "Y (controls.hold)");
    CHECK(boundControlLabel(config, nullptr, 0, true, "unknown") == "controls.unbound");
}

TEST_CASE("action tokens do not rewrite ordinary letters or action names", "[controls][prompts]") {
    s32 owner = -1;
    test::ScriptedControlLabels labels;
    labels.answer = [&](s32 player, std::string_view action) {
        owner = player;
        return std::string(action == "menuStart" ? "F2" : "RMB");
    };
    CHECK(controlText("Press {bind:menuStart}, then {bind:menuSelect}", &labels, 2) ==
          "Press F2, then RMB");
    CHECK(owner == 2);
    CHECK(controlText("A key, B: buy, START, X-Ray", &labels, 1) == "A key, B: buy, START, X-Ray");
    CHECK(controlText("Press {bind:unfinished", &labels, 1) == "Press {bind:unfinished");
}

TEST_CASE("without live labels a prompt names the default bindings", "[controls][prompts]") {
    const GameConfig defaults;
    // A shared prompt and the first player read the keyboard's; the others their pads'.
    CHECK(controlLabel(nullptr, -1, "attack") ==
          boundControlLabel(defaults, nullptr, 0, true, "attack"));
    CHECK(controlLabel(nullptr, 0, "attack") ==
          boundControlLabel(defaults, nullptr, 0, true, "attack"));
    CHECK(controlLabel(nullptr, 2, "attack") ==
          boundControlLabel(defaults, nullptr, 2, false, "attack"));
    CHECK(controlText("Press {bind:menuSelect}", nullptr, 0) ==
          "Press " + boundControlLabel(defaults, nullptr, 0, true, "menuSelect"));
    // Labels a test leaves unanswered name nothing, rather than the defaults.
    const test::ScriptedControlLabels silent;
    CHECK(controlLabel(&silent, 0, "attack").empty());
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
    CHECK(prompts.label(input, config, nullptr, 0, "menuSelect") == "Enter");
    pad.buttons[static_cast<usize>(PadButton::A)] = true;
    input.setPad(0, pad);
    prompts.update(input, config);
    CHECK(prompts.label(input, config, nullptr, 0, "menuSelect") == "A");
    input.beginPoll();
    input.setKey(Key::W, true);
    prompts.update(input, config);
    CHECK(prompts.label(input, config, nullptr, 0, "menuSelect") == "Enter");
    config.controls[2].device = "keyboard";
    config.controls[2].customized = true;
    config.controls[2].menu.select = {Key::F3};
    input.beginPoll();
    input.setKey(Key::F3, true);
    prompts.update(input, config);
    CHECK(prompts.lastPlayer() == 2);
    CHECK(prompts.label(input, config, nullptr, 2, "menuSelect") == "F3");
    CHECK(prompts.label(input, config, nullptr, 0, "menuSelect") == "A");
    config.controls[0].device = "test";
    input.setPad(0, {});
    CHECK(prompts.label(input, config, nullptr, 0, "menuSelect") == "A");
}

TEST_CASE("the pointer hides while a pad plays and returns with the mouse or keyboard",
          "[hud][prompts][cursor]") {
    GameConfig config;
    Input input;
    PadSnapshot pad;
    pad.connected = true;
    pad.guid = "test";
    input.setPad(0, pad);
    input.beginPoll();
    PromptDevices prompts;
    prompts.update(input, config);
    CHECK_FALSE(prompts.cursorHidden());
    // A pad press puts the pointer away.
    pad.buttons[static_cast<usize>(PadButton::A)] = true;
    input.setPad(0, pad);
    prompts.update(input, config);
    CHECK(prompts.cursorHidden());
    pad.buttons[static_cast<usize>(PadButton::A)] = false;
    input.setPad(0, pad);
    input.beginPoll();
    prompts.update(input, config);
    CHECK(prompts.cursorHidden()); // and it stays away while nothing else is touched
    // A key brings it back; so does the mouse moving over the window.
    input.beginPoll();
    input.setKey(Key::W, true);
    prompts.update(input, config);
    CHECK_FALSE(prompts.cursorHidden());
    input.beginPoll();
    pad.buttons[static_cast<usize>(PadButton::A)] = false;
    input.setPad(0, pad);
    input.beginPoll();
    pad.buttons[static_cast<usize>(PadButton::A)] = true;
    input.setPad(0, pad);
    prompts.update(input, config);
    CHECK(prompts.cursorHidden());
    input.beginPoll();
    input.setPointer({0.2f, 0.2f, true, false});
    prompts.update(input, config);
    input.beginPoll();
    input.setPointer({0.3f, 0.2f, true, false});
    prompts.update(input, config);
    CHECK_FALSE(prompts.cursorHidden());
    // The keyboard assigned to a player by choice keeps the pointer whatever the pads do.
    config.controls[0].device = "keyboard";
    config.controls[0].customized = true;
    input.beginPoll();
    pad.buttons[static_cast<usize>(PadButton::A)] = false;
    input.setPad(0, pad);
    input.beginPoll();
    pad.buttons[static_cast<usize>(PadButton::A)] = true;
    input.setPad(0, pad);
    prompts.update(input, config);
    CHECK_FALSE(prompts.cursorHidden());
}

} // namespace
