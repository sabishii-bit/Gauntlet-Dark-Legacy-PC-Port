#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/PowerupEffects.h"
#include "game/screens/PowerupSelector.h"
#include "game/screens/StatusBox.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using State = PowerupSelector::State;

TEST_CASE("the inventory label rests immediately above the runestones",
          "[selector][inventory-label]") {
    Inventory inventory;
    inventory.powerups[0] = {30, 9, 0, 4, false};
    PowerupSelector selector;
    selector.step(SelectorInput{.up = true}, inventory, 32);
    selector.step({}, inventory, 1);
    REQUIRE(selector.showing());
    CHECK(PowerupSelector::kLabelX == 24);
    CHECK(selector.labelY(StatusBoxPainter::kY) == 291);
    const f32 bottom =
        static_cast<f32>(selector.labelY(StatusBoxPainter::kY)) + 32 * PowerupSelector::kLabelScale;
    CHECK(bottom < StatusBoxPainter::kRuneY);
    CHECK(StatusBoxPainter::kRuneY - bottom < 1.0f);
}

SelectorInput press(bool up, bool down, bool left, bool right) {
    return SelectorInput{up, down, left, right};
}

TEST_CASE("the challenge selector keeps Stop Time carried but refuses to activate it",
          "[selector][stop-time][secret]") {
    Inventory inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kStopTime, 0, 45);
    inventory.addPowerup(powerup::kSpecial, powerup::kLevitation, 0, 60);
    PowerupSelector selector;
    selector.focus(inventory, powerup::kSpecial, powerup::kStopTime);
    selector.step(SelectorInput{.up = true}, inventory, 32, true);
    selector.step({}, inventory, 1, true);
    REQUIRE(selector.showing());
    CHECK(selector.step(SelectorInput{.up = true}, inventory, 1, true) == SelectorCue::Switched);
    REQUIRE_FALSE(inventory.powerups[0].on); // Switching off is always safe.
    CHECK(selector.step(SelectorInput{.up = true}, inventory, 1, true) == SelectorCue::None);
    CHECK_FALSE(inventory.powerups[0].on);
    CHECK(inventory.powerups[0].strength == 45);
    CHECK(selector.step(SelectorInput{.right = true}, inventory, 1, true) == SelectorCue::Moved);
    CHECK(selector.selection() == 1);
    CHECK(selector.step(SelectorInput{.up = true}, inventory, 1, true) == SelectorCue::Switched);
    CHECK_FALSE(inventory.powerups[1].on);
    CHECK(selector.step(SelectorInput{.up = true}, inventory, 1, true) == SelectorCue::Switched);
    CHECK(inventory.powerups[1].on);
    selector.focus(inventory, powerup::kSpecial, powerup::kStopTime);
    CHECK(selector.step(SelectorInput{.up = true}, inventory, 1, false) == SelectorCue::Switched);
    CHECK(inventory.powerups[0].on);
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

TEST_CASE("pickup focus selects new renewed and replaced slots without opening the selector",
          "[game][screens][selector]") {
    Inventory inventory;
    PowerupSelector selector;
    inventory.addPowerup(powerup::kSpecial, powerup::kGrowth, 0, 30);
    inventory.addPowerup(powerup::kSpeed, 0, 2, 30);
    selector.focus(inventory, powerup::kSpecial, powerup::kGrowth);
    REQUIRE(selector.selection() == 0); // Not the last occupied slot.
    REQUIRE(selector.state() == State::Closed);
    REQUIRE(selector.step(SelectorInput{.up = true}, inventory, 32) == SelectorCue::Opened);
    selector.focus(inventory, powerup::kSpeed, 0);
    REQUIRE(selector.state() == State::SlidingIn);
    REQUIRE(selector.slide() == PowerupSelector::kSlide);
    selector.step({}, inventory, 1);
    REQUIRE(selector.showing());
    inventory.addPowerup(powerup::kSpecial, powerup::kGrowth, 0, 30);
    selector.focus(inventory, powerup::kSpecial, powerup::kGrowth);
    REQUIRE(selector.selection() == 0);
    REQUIRE(selector.showing());
    REQUIRE(selector.step(SelectorInput{.up = true}, inventory, 1) == SelectorCue::Switched);
    REQUIRE_FALSE(inventory.powerups[0].on);
    REQUIRE(inventory.powerups[1].on);
    selector.focus(inventory, powerup::kArmor, 0); // No such acquired item.
    REQUIRE(selector.selection() == 0);
    selector.step(SelectorInput{.down = true}, inventory, 32);
    inventory.powerups[0] = {};
    inventory.addPowerup(powerup::kArmor, powerup::kReflectShield, 0, 60);
    selector.focus(inventory, powerup::kArmor, powerup::kReflectShield);
    REQUIRE(selector.state() == State::SlidingOut);
    selector.step({}, inventory, 1);
    selector.step(SelectorInput{.up = true}, inventory, 32);
    REQUIRE(selector.selection() == 0);
}

TEST_CASE("usage follows selection then newest active pickup and ignores disabled or expired items",
          "[game][screens][selector]") {
    Inventory inventory;
    PowerupSelector selector;
    inventory.addPowerup(powerup::kSpecial, powerup::kGrowth, 0, 30);
    selector.focus(inventory, powerup::kSpecial, powerup::kGrowth);
    inventory.addPowerup(powerup::kSpeed, 0, 2, 30);
    selector.focus(inventory, powerup::kSpeed, 0);
    inventory.addPowerup(powerup::kArmor, powerup::kInvulnerable, 0, 30);
    selector.focus(inventory, powerup::kArmor, powerup::kInvulnerable);
    REQUIRE(selector.usageSlot(inventory) == 2);
    inventory.powerups[2].on = false;
    REQUIRE(selector.usageSlot(inventory) == 1);
    inventory.powerups[1].strength = 0;
    REQUIRE(selector.usageSlot(inventory) == 0);
    inventory.powerups[0].strength = -1; // Permanent is not a negative countdown.
    REQUIRE(selector.usageSlot(inventory) == -1);
    selector.close();
    REQUIRE(selector.selection() == -1);
}

TEST_CASE("usage distinguishes charges from timed stat boosts and permanent items",
          "[game][screens][selector]") {
    const PowerupSlot speed{20.26f, powerup::kSpeed, 5, 0, true};
    REQUIRE_FALSE(PowerupSelector::charged(speed));
    REQUIRE(PowerupSelector::remaining(speed) == 20.26f);
    PowerupSlot shots{-1, powerup::kWeapon, 4, powerup::kSuperShot, true};
    REQUIRE(PowerupSelector::charged(shots));
    REQUIRE(PowerupSelector::remaining(shots) == 4);
    shots.charge = -1;
    REQUIRE_FALSE(PowerupSelector::remaining(shots));
    shots.charge = 0;
    REQUIRE_FALSE(PowerupSelector::remaining(shots));
    shots.charge = 2.75f;
    REQUIRE(PowerupSelector::remaining(shots) == 2);
    shots.on = false;
    REQUIRE_FALSE(PowerupSelector::remaining(shots));
    REQUIRE(PowerupSelector::charged(
        PowerupSlot{-1, powerup::kWeapon, 3, powerup::kThunderHammer, true}));
    REQUIRE(PowerupSelector::charged(
        PowerupSlot{-1, powerup::kSpecial, 3, powerup::kFireBreath, true}));
}

} // namespace
