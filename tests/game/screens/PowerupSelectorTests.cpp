#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/screens/PowerupSelector.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using State = PowerupSelector::State;

SelectorInput press(bool up, bool down, bool left, bool right) {
    return SelectorInput{up, down, left, right};
}

TEST_CASE("the selector opens on a carried powerup, goes round them, and switches them",
          "[game][screens][selector]") {
    Inventory inventory;
    PowerupSelector selector;
    // With nothing carried, up does nothing.
    REQUIRE(selector.step(press(true, false, false, false), inventory, 2) == SelectorCue::None);
    REQUIRE(selector.state() == State::Closed);

    inventory.powerups[1] = PowerupSlot{30.0f, 9, 0.0f, 0x4, true};
    inventory.powerups[4] = PowerupSlot{30.0f, 5, 0.0f, 0x80000, true};
    REQUIRE(selector.step(press(true, false, false, false), inventory, 2) == SelectorCue::Opened);
    REQUIRE(selector.state() == State::SlidingIn);
    REQUIRE(selector.selection() == 4); // the last carried comes up first
    REQUIRE_FALSE(selector.showing());
    // The label takes its ride of 128 at four a tick before it shows.
    s32 steps = 0;
    while (!selector.showing() && steps < 40) {
        selector.step(SelectorInput{}, inventory, 2);
        ++steps;
    }
    REQUIRE(selector.showing());
    REQUIRE(steps >= 15);
    REQUIRE(selector.labelY(320) == 320 - PowerupSelector::kLabelRise);

    REQUIRE(selector.step(press(false, false, true, false), inventory, 2) == SelectorCue::Moved);
    REQUIRE(selector.selection() == 1);
    REQUIRE(selector.step(press(false, false, false, true), inventory, 2) == SelectorCue::Moved);
    REQUIRE(selector.selection() == 4);
    // Up takes the named one off, and again puts it back on.
    REQUIRE(selector.step(press(true, false, false, false), inventory, 2) == SelectorCue::Switched);
    REQUIRE_FALSE(inventory.powerups[4].on);
    selector.step(press(true, false, false, false), inventory, 2);
    REQUIRE(inventory.powerups[4].on);

    // The named one running out hands over to the one before; the last closes it.
    inventory.powerups[4].strength = 0.0f;
    REQUIRE(selector.step(SelectorInput{}, inventory, 2) == SelectorCue::Moved);
    REQUIRE(selector.selection() == 1);
    inventory.powerups[1].strength = 0.0f;
    REQUIRE(selector.step(SelectorInput{}, inventory, 2) == SelectorCue::Closed);
    REQUIRE(selector.state() == State::SlidingOut);
    REQUIRE_FALSE(selector.showing());

    // Down closes it too.
    inventory.powerups[1].strength = 5.0f;
    selector.close();
    selector.step(press(true, false, false, false), inventory, 2);
    while (!selector.showing()) {
        selector.step(SelectorInput{}, inventory, 2);
    }
    REQUIRE(selector.step(press(false, true, false, false), inventory, 2) == SelectorCue::Closed);
    while (selector.state() != State::Closed) {
        selector.step(SelectorInput{}, inventory, 2);
    }
    REQUIRE(selector.selection() == 1); // it remembers where it was
}

} // namespace
