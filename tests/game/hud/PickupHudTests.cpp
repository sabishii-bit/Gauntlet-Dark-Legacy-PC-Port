#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "game/characters/ClassData.h"
#include "game/characters/Relics.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/PickupHud.h"
#include "game/level/SecretChallenge.h"

namespace {

using namespace gdl;
using namespace gdl::game;

const PickupCount& first(const PickupHud& hud, s32 player) {
    return hud.counts(player)[0];
}

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
    for (s32 i = 0; i < 30; ++i) {
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
    REQUIRE_FALSE(first(hud, 0).showing());
    hud.showCount(0, PickupHud::crystalIcon(1), 3, 15);
    REQUIRE(first(hud, 0).showing());
    REQUIRE(first(hud, 0).icon == "SM_CRYSTAL_ORA");
    REQUIRE(first(hud, 0).count == 3);
    REQUIRE(first(hud, 0).total == 15);
    REQUIRE_FALSE(first(hud, 1).showing());
    // A realm already completed keeps a record under nought; it reads as full, not "-1".
    hud.showCount(1, PickupHud::crystalIcon(2), -1, 100);
    REQUIRE(first(hud, 1).count == 100);
    REQUIRE(first(hud, 1).total == 100);
    hud.step(0, 2.9f);
    REQUIRE(first(hud, 0).showing());
    hud.step(0, 0.2f);
    REQUIRE_FALSE(first(hud, 0).showing());
    // A later pickup starts the time over; clearing drops it.
    hud.showCount(0, PickupHud::crystalIcon(1), 4, 15);
    hud.step(0, 1.0f);
    hud.showCount(0, PickupHud::crystalIcon(1), 5, 15);
    hud.step(0, 2.5f);
    REQUIRE(first(hud, 0).showing());
    REQUIRE(first(hud, 0).count == 5);
    hud.clear();
    REQUIRE_FALSE(first(hud, 0).showing());
    // Out-of-range players are ignored; realms outside the tables have no icon.
    hud.showCount(9, "X", 1, 1);
    REQUIRE(hud.counts(9).empty());
    REQUIRE(PickupHud::crystalIcon(0).empty());
    REQUIRE(PickupHud::crystalIcon(8) == "SM_CRYSTAL_BLA");
    REQUIRE(PickupHud::crystalIcon(9).empty());
    // Without the boxes' art, drawing is a no-op.
    test::FakeRenderDevice device;
    DarkLegacyHudPainter look;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    hud.addCard(0, PickupHud::kCrystalCard);
    hud.showCount(0, PickupHud::crystalIcon(1), 1, 15);
    look.drawPickups(canvas, hud, {});
    canvas.end();
    REQUIRE(device.draws.empty());
}

TEST_CASE("pickup counts of different icons stack and one icon updates in place",
          "[game][screens][hud]") {
    PickupHud hud;
    hud.showCount(0, PickupHud::crystalIcon(1), 3, 15);
    hud.showCount(0, "SM_FEATHER", 1, 4);
    hud.step(0, 1.0f);
    hud.showCount(0, PickupHud::crystalIcon(1), 4, 15);
    REQUIRE(hud.counts(0)[0].count == 4);
    REQUIRE(hud.counts(0)[0].secondsLeft == PickupHud::kCountSeconds);
    REQUIRE(hud.counts(0)[1].icon == "SM_FEATHER");
    REQUIRE(hud.counts(0)[1].secondsLeft < PickupHud::kCountSeconds);
    REQUIRE_FALSE(hud.counts(1)[0].showing());
    // A full stack gives way to the oldest, and a lapsed count frees its place.
    hud.showCount(0, "SM_CLAW", 1, 4);
    hud.step(0, 1.0f);
    hud.showCount(0, "SM_FANGS", 2, 4);
    REQUIRE(hud.counts(0)[1].icon == "SM_FANGS");
    REQUIRE(hud.counts(0)[0].icon == PickupHud::crystalIcon(1));
    hud.step(0, 5.0f);
    hud.showCount(0, "SM_CLAW", 1, 4);
    REQUIRE(hud.counts(0)[0].icon == "SM_CLAW");
}

TEST_CASE("the count stack rests low and sits above a label while one is up",
          "[game][screens][hud]") {
    constexpr s32 kLabelTop = 291;
    REQUIRE(PickupHud::countRowY(std::nullopt) == PickupHud::kCountRestY);
    const s32 above = PickupHud::countRowY(kLabelTop);
    REQUIRE(above + PickupHud::kCountHeight + PickupHud::kCountLabelGap == kLabelTop);
    REQUIRE(above < PickupHud::kCountRestY);
}

TEST_CASE("every icon a pickup count shows is on the roster a look takes its pictures from",
          "[game][screens][hud]") {
    const std::vector<std::string> icons = PickupHud::countIcons();
    const auto listed = [&](std::string_view icon) {
        return std::ranges::find(icons, icon) != icons.end();
    };
    // Each realm's crystal, each kind of gargoyle piece, and the coins of the class each secret
    // level's challenge unlocks, the level's own pictures: Sumner's by a name of their own.
    for (s32 realm = 1; realm < static_cast<s32>(PickupHud::kCrystalIcons.size()); ++realm) {
        CAPTURE(realm);
        CHECK(listed(PickupHud::crystalIcon(realm)));
    }
    CHECK(PickupHud::kGargoyleIcons ==
          std::array<std::string_view, 3>{"SM_FANGS", "SM_FEATHERS", "SM_CLAWS"});
    for (const std::string_view piece : PickupHud::kGargoyleIcons) {
        CHECK(listed(piece));
    }
    usize secretLevels = 0;
    for (; SecretChallenge::classFor(static_cast<s32>(secretLevels)) >= 0; ++secretLevels) {
        CHECK(listed(PickupHud::challengeCoinIcon(
            SecretChallenge::classFor(static_cast<s32>(secretLevels)))));
    }
    CHECK(secretLevels == 9);
    CHECK(PickupHud::challengeCoinIcon(kSumnerClass) == "16_SUM");
    CHECK(PickupHud::challengeCoinIcon(kJackalClass) == "16_JACCOIN");
    CHECK(PickupHud::challengeCoinIcon(kMinotaurClass) == "16_MINCOIN");
    // Nothing else, and each once.
    CHECK(icons.size() ==
          PickupHud::kCrystalIcons.size() - 1 + Relics::kGargoyleKinds + secretLevels);
    std::vector<std::string> sorted = icons;
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());
    CHECK_FALSE(listed(""));
}

} // namespace
