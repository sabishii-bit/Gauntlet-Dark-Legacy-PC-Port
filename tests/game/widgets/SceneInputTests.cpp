#include <array>
#include <initializer_list>
#include <tuple>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/platform/Input.h"

#include "game/config/ControlProfiles.h"
#include "game/config/GameConfig.h"
#include "game/widgets/MenuInput.h"
#include "game/widgets/SceneInput.h"

namespace {

using namespace gdl;
using gdl::game::GameConfig;
using gdl::game::kInputSlots;
using gdl::game::MenuInput;
using gdl::game::SceneInput;

/** A connected pad holding `buttons` down. */
PadSnapshot padHolding(std::initializer_list<PadButton> buttons) {
    PadSnapshot pad;
    pad.connected = true;
    for (const PadButton button : buttons) {
        pad.buttons[static_cast<usize>(button)] = true;
    }
    return pad;
}

/** Every field of a reading, so that two readings compare whole. */
auto fieldsOf(const MenuInput& menu) {
    return std::tie(menu.devices, menu.pointer, menu.pointerScroll, menu.pointerNormalized,
                    menu.pointerPressed, menu.pointerHeld, menu.pointerBack, menu.up, menu.down,
                    menu.left, menu.right, menu.select, menu.back, menu.start, menu.upHeld,
                    menu.downHeld, menu.leftHeld, menu.rightHeld, menu.typed, menu.erase,
                    menu.escape, menu.buttonPressed);
}

bool sameReading(const MenuInput& left, const MenuInput& right) {
    return fieldsOf(left) == fieldsOf(right);
}

TEST_CASE("a name being typed borrows the keyboard and leaves every pad with its slot",
          "[game][menu][input]") {
    const GameConfig config;
    Input input;
    input.setKey(Key::W, true);
    input.setKey(Key::Enter, true);
    input.addTypedChar('w');
    input.setPad(1, padHolding({PadButton::DpadUp}));
    input.setPad(2, padHolding({PadButton::DpadDown}));
    const SceneInput scene{input, config};

    // Slot 2 plays on its pad alone, yet takes the keyboard while it types.
    const auto padTypist = scene.typing(2);
    CHECK(padTypist[2].typed == "w");
    CHECK_FALSE(padTypist[2].up); // W types instead of steering
    CHECK(padTypist[2].select);   // Enter still confirms
    CHECK(padTypist[2].down);     // its own pad still steers
    CHECK(padTypist[0].typed.empty());
    CHECK_FALSE(padTypist[0].up);
    CHECK_FALSE(padTypist[0].select);
    CHECK(padTypist[1].up);
    CHECK_FALSE(padTypist[1].select);
    CHECK(padTypist[1].typed.empty());
    CHECK_FALSE(padTypist[3].any());

    // The keyboard's own slot types the same way, and the pads stay where they were.
    const auto keyboardTypist = scene.typing(0);
    CHECK(keyboardTypist[0].typed == "w");
    CHECK_FALSE(keyboardTypist[0].up);
    CHECK(keyboardTypist[0].select);
    CHECK(keyboardTypist[1].up);
    CHECK(keyboardTypist[2].down);
    CHECK_FALSE(keyboardTypist[2].select);
    CHECK(keyboardTypist[2].typed.empty());

    // Lent to nobody, the keyboard steers its own slot and nothing is typed.
    const auto nobody = scene.typing(-1);
    CHECK(nobody[0].up);
    CHECK(nobody[0].select);
    CHECK(nobody[0].typed.empty());
    CHECK(nobody[1].up);
    CHECK(nobody[2].down);
    CHECK_FALSE(nobody[2].select);
    CHECK(nobody[2].typed.empty());
    for (usize slot = 0; slot < kInputSlots; ++slot) {
        CAPTURE(slot);
        CHECK(sameReading(nobody[slot], scene.slot(static_cast<s32>(slot))));
    }
}

TEST_CASE("only the admitted slots join, and only by Start", "[game][menu][input]") {
    const GameConfig config;
    Input input;
    input.setKey(Key::Enter, true); // Start and Select for the keyboard's slot
    input.setPad(1, padHolding({PadButton::Start, PadButton::A}));
    input.setPad(2, padHolding({PadButton::A}));
    input.setPad(3, padHolding({PadButton::Start}));
    const SceneInput scene{input, config};
    REQUIRE(scene.slot(0).start);
    REQUIRE(scene.slot(2).select);
    REQUIRE(scene.slot(3).start);

    const auto joining = scene.joining({false, true, true, false});
    CHECK_FALSE(joining[0].start);
    CHECK(joining[1].start);
    CHECK_FALSE(joining[2].start);
    CHECK_FALSE(joining[3].start);
    for (const MenuInput& menu : joining) {
        CHECK_FALSE(menu.select);
        CHECK_FALSE(menu.buttonPressed);
    }
    const auto everyone = scene.joining({true, true, true, true});
    CHECK(everyone[0].start);
    CHECK(everyone[1].start);
    CHECK_FALSE(everyone[2].start);
    CHECK(everyone[3].start);
}

TEST_CASE("a slot's binding press counts its own keys and pad, never the mouse or another pad",
          "[game][menu][input][mouse]") {
    const std::array keys{Key::MouseLeft, Key::MouseRight, Key::Backspace};
    const std::array buttons{PadButton::X};
    const GameConfig config;
    Input input;
    const SceneInput scene{input, config};

    input.setKey(Key::MouseLeft, true);
    input.setKey(Key::MouseRight, true);
    CHECK_FALSE(scene.pressed(0, keys, buttons));
    input.setKey(Key::Backspace, true);
    CHECK(scene.pressed(0, keys, buttons));
    CHECK_FALSE(scene.pressed(1, keys, buttons)); // the keyboard is not slot 1's

    input.setPad(2, padHolding({PadButton::X}));
    CHECK_FALSE(scene.pressed(1, keys, buttons));
    CHECK(scene.pressed(2, keys, buttons));
    input.setPad(1, padHolding({PadButton::A}));
    CHECK_FALSE(scene.pressed(1, keys, buttons)); // a button the binding does not name

    // A keyboard given to another slot presses for that slot alone.
    GameConfig moved;
    moved.controls[2].device = "keyboard";
    const SceneInput reassigned{input, moved};
    CHECK(reassigned.pressed(2, keys, {}));
    CHECK_FALSE(reassigned.pressed(0, keys, {}));
}

TEST_CASE("the recovery key is Escape and the quit keys are the settings' own",
          "[game][menu][input]") {
    GameConfig config;
    config.menu.escape = {Key::Q};
    config.controls[0].customized = true;
    config.controls[0].menu.escape = {Key::F1};
    Input input;
    const SceneInput scene{input, config};
    CHECK_FALSE(scene.quitPressed());
    CHECK_FALSE(scene.recoveryPressed());

    input.setKey(Key::F1, true);
    CHECK(scene.slot(0).escape);
    CHECK_FALSE(scene.quitPressed()); // a slot's own escape is not the settings' quit

    input.beginPoll();
    input.setKey(Key::F1, false);
    input.setKey(Key::Q, true);
    CHECK(scene.quitPressed());
    CHECK_FALSE(scene.recoveryPressed());
    CHECK_FALSE(scene.slot(0).escape);

    input.beginPoll();
    input.setKey(Key::Q, false);
    input.setKey(Key::Escape, true);
    CHECK(scene.recoveryPressed());
    CHECK_FALSE(scene.quitPressed());
    // The settings are read when asked, so a change made during the update is seen.
    config.menu.escape = {Key::Escape};
    CHECK(scene.quitPressed());

    // Neither needs a slot to hold the keyboard.
    GameConfig unassigned;
    for (auto& profile : unassigned.controls) {
        profile.device = "none";
    }
    const SceneInput nobody{input, unassigned};
    CHECK(nobody.recoveryPressed());
    CHECK(nobody.quitPressed());
}

TEST_CASE("scene input reads what the menu readers read from the same devices",
          "[game][menu][input]") {
    GameConfig config;
    config.controls[1].device = "keyboard";
    config.controls[3].device = "none";
    Input input;
    input.setKey(Key::Enter, true);
    input.setKey(Key::Escape, true);
    input.addTypedChar('x');
    input.setPad(0, padHolding({PadButton::Start}));
    input.setPad(2, padHolding({PadButton::A, PadButton::DpadLeft}));
    input.setPointer({0.25f, 0.75f, true, true});
    const SceneInput scene{input, config};
    CHECK(&scene.devices() == &input);
    CHECK(&scene.config() == &config);

    const auto slots = scene.slots();
    for (s32 slot = 0; slot < static_cast<s32>(kInputSlots); ++slot) {
        CAPTURE(slot);
        const auto source = scene.source(slot);
        const auto expected = gdl::game::playerInputSource(input, config, slot);
        CHECK(source.keyboard == expected.keyboard);
        CHECK(source.pad == expected.pad);
        CHECK(source.text == expected.text);
        const auto menu = gdl::game::readPlayerMenuInput(input, config, slot);
        CHECK(sameReading(scene.slot(slot), menu));
        CHECK(sameReading(slots[static_cast<usize>(slot)], menu));
        CHECK(sameReading(scene.movie(slot), gdl::game::readMovieMenuInput(input, config, slot)));
        CHECK(scene.movieVote(slot) == gdl::game::readOnlineMovieSkipInput(input, config, slot));
    }
    CHECK(sameReading(scene.shared(), gdl::game::readSharedMenuInput(input, config)));
    CHECK(sameReading(scene.movie(), gdl::game::readMovieMenuInput(input, config)));
    for (s32 owner = -1; owner <= static_cast<s32>(kInputSlots); ++owner) {
        CAPTURE(owner);
        CHECK(sameReading(scene.pause(owner), gdl::game::readPauseMenuInput(input, config, owner)));
    }
}

} // namespace
