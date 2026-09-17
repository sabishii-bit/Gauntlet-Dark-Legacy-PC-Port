#include <catch2/catch_test_macros.hpp>

#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "game/screens/PickupHud.h"
#include "game/screens/StatusBox.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("a pickup card rises over the box, holds, and falls away", "[game][screens][hud]") {
    PickupHud hud;
    hud.addCard(1, PickupHud::kCrystalCard);
    REQUIRE(hud.cards().size() == 1);
    REQUIRE(hud.cards()[0].player == 1);
    REQUIRE(hud.cards()[0].texture == "CRYSTAL");
    REQUIRE(hud.cards()[0].y == PickupHud::kCardStartY);
    REQUIRE(hud.cards()[0].state == PickupCard::State::Rising);
    // A pixel a tick up to the bar over the box.
    hud.step(40, 0.0f);
    REQUIRE(hud.cards()[0].y == PickupHud::kCardStartY - 40);
    hud.step(60, 0.0f);
    REQUIRE(hud.cards()[0].y == PickupHud::kCardRestY);
    REQUIRE(hud.cards()[0].state == PickupCard::State::Holding);
    // Ninety ticks there, then back down and off.
    hud.step(PickupHud::kCardHoldTicks - 1, 0.0f);
    REQUIRE(hud.cards()[0].state == PickupCard::State::Holding);
    hud.step(1, 0.0f);
    REQUIRE(hud.cards()[0].state == PickupCard::State::Falling);
    hud.step(PickupHud::kCardEndY - PickupHud::kCardRestY - 1, 0.0f);
    REQUIRE(hud.cards().size() == 1);
    hud.step(1, 0.0f);
    REQUIRE(hud.cards().empty());
    // Only so many at once, and only for real slots.
    for (int i = 0; i < 30; ++i) {
        hud.addCard(0, "KEY");
    }
    REQUIRE(hud.cards().size() == PickupHud::kMostCards);
    hud.addCard(7, "KEY");
    hud.addCard(-1, "KEY");
    REQUIRE(hud.cards().size() == PickupHud::kMostCards);
    hud.clearPlayer(0);
    REQUIRE(hud.cards().empty());
}

TEST_CASE("a pickup count shows for three seconds", "[game][screens][hud]") {
    PickupHud hud;
    REQUIRE_FALSE(hud.count(0).showing());
    hud.showCount(0, PickupHud::crystalIcon(1), 3, 15);
    REQUIRE(hud.count(0).showing());
    REQUIRE(hud.count(0).icon == "SM_CRYSTAL_ORA");
    REQUIRE(hud.count(0).count == 3);
    REQUIRE(hud.count(0).total == 15);
    REQUIRE_FALSE(hud.count(1).showing());
    hud.step(0, 2.9f);
    REQUIRE(hud.count(0).showing());
    hud.step(0, 0.2f);
    REQUIRE_FALSE(hud.count(0).showing());
    // A later pickup starts the time over; clearing drops it.
    hud.showCount(0, PickupHud::crystalIcon(1), 4, 15);
    hud.step(0, 1.0f);
    hud.showCount(0, PickupHud::crystalIcon(1), 5, 15);
    hud.step(0, 2.5f);
    REQUIRE(hud.count(0).showing());
    REQUIRE(hud.count(0).count == 5);
    hud.clear();
    REQUIRE_FALSE(hud.count(0).showing());
    // Out-of-range players are ignored; realms outside the tables have no icon.
    hud.showCount(9, "X", 1, 1);
    REQUIRE_FALSE(hud.count(9).showing());
    REQUIRE(PickupHud::crystalIcon(0).empty());
    REQUIRE(PickupHud::crystalIcon(8) == "SM_CRYSTAL_BLA");
    REQUIRE(PickupHud::crystalIcon(9).empty());
    // Without the boxes' art, drawing is a no-op.
    test::FakeRenderDevice device;
    StatusBoxPainter boxes;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    hud.addCard(0, PickupHud::kCrystalCard);
    hud.showCount(0, PickupHud::crystalIcon(1), 1, 15);
    hud.draw(canvas, boxes);
    canvas.end();
    REQUIRE(device.draws.empty());
}

} // namespace
