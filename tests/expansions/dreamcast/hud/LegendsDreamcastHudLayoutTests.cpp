#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Strings.h"
#include "engine/core/Types.h"

#include "RecordedQuads.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"
#include "game/bosses/LegendItems.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/HudView.h"
#include "game/hud/PickupHud.h"
#include "game/hud/RuneMeter.h"
#include "game/hud/StatusBox.h"
#include "game/level/BossKinds.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/ItemPickup.h"
#include "game/shop/InventoryPanel.h"
#include "game/widgets/ScrollBox.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Layout = LegendsDreamcastHudLayout;
using test::checkRect;

/** Seat 1's column. */
constexpr s32 kX = 120;

/** The legend items of `realms`, a bit per realm, as Relics keeps them. */
u16 legendsOf(std::initializer_list<s32> realms) {
    u16 legends = 0;
    for (const s32 realm : realms) {
        legends = static_cast<u16>(legends | (1U << static_cast<u32>(realm)));
    }
    return legends;
}

} // namespace

TEST_CASE("Dreamcast seats keep their columns whatever the party size",
          "[legends][dreamcast-hud]") {
    // 8c14bbb8 and 8c14bbc0: four columns, whoever plays, each box 96 across.
    constexpr std::array<s32, 4> kColumns{8, 120, 296, 408};
    constexpr std::array<s32, 4> kCentres{56, 168, 344, 456};
    for (usize seat = 0; seat < kColumns.size(); ++seat) {
        CAPTURE(seat);
        CHECK(dreamcastSeatX(seat) == kColumns.at(seat));
        CHECK(dreamcastSeatCentre(seat) == kCentres.at(seat));
        CHECK(dreamcastSeatCentre(seat) == dreamcastSeatX(seat) + 48);
    }
    // The boxes stand symmetric about the frame's middle, an 80-unit gap between the second
    // and the third.
    CHECK(dreamcastSeatCentre(0) + dreamcastSeatCentre(3) == 512);
    CHECK(dreamcastSeatCentre(1) + dreamcastSeatCentre(2) == 512);
    CHECK(dreamcastSeatX(2) - (dreamcastSeatX(1) + 96) == 80);
}

TEST_CASE("the Dreamcast turbo arch picks its texture by zone and crops from its centre",
          "[legends][dreamcast-hud]") {
    // FUN_8c0c13e0: below 0.3, 0.6 and 0.9999 of the meter green, yellow and red, the half-width
    // opening from 0 to 22, from 22 to 40 and from 40 to 52 across them, about texel 56; the
    // blit stands a unit right of the texel's own place.
    struct Cropped {
        f32 shown = 0;
        std::string_view art;
        f32 half = 0;
    };
    constexpr std::array<Cropped, 5> kCropped{{
        {.shown = 15, .art = "TRBO_GRADNT_GREEN", .half = 11},
        {.shown = 30, .art = "TRBO_GRADNT_YELO", .half = 22},
        {.shown = 45, .art = "TRBO_GRADNT_YELO", .half = 31},
        {.shown = 60, .art = "TRBO_GRADNT_RED", .half = 40},
        {.shown = 80, .art = "TRBO_GRADNT_RED", .half = 46},
    }};
    for (const Cropped& expected : kCropped) {
        CAPTURE(expected.shown);
        const LegendsDreamcastTurboArch arch = dreamcastTurbo(expected.shown, kX);
        CHECK(arch.art == expected.art);
        CHECK_FALSE(arch.full);
        checkRect(arch.area,
                  {static_cast<f32>(kX - 8 + 57) - expected.half, 306, 2 * expected.half, 32});
        checkRect(arch.uv, {(56 - expected.half) / 128, 0, 2 * expected.half / 128, 1});
    }

    // From 0.9999 of the meter the whole arch stands in its own place, and glows.
    for (const f32 shown : {99.99f, 100.0f, 250.0f}) {
        CAPTURE(shown);
        const LegendsDreamcastTurboArch arch = dreamcastTurbo(shown, kX);
        CHECK(arch.art == "TRBO_FULL");
        CHECK(arch.full);
        checkRect(arch.area, {kX - 8, 306, 128, 32});
        checkRect(arch.uv, {0, 0, 1, 1});
    }
    const LegendsDreamcastTurboArch almost = dreamcastTurbo(99.98f, kX);
    CHECK(almost.art == "TRBO_GRADNT_RED");
    CHECK_FALSE(almost.full);

    // The blit keeps whole units: a half-width between them, 7 1/3 at a tenth of the meter, is
    // cut to 7 in the width, and the truncated left edge falls on the unit to its right, while
    // the picture is still sampled across the half-width as it is.
    const LegendsDreamcastTurboArch between = dreamcastTurbo(10, kX);
    CHECK(between.art == "TRBO_GRADNT_GREEN");
    checkRect(between.area, {kX - 8 + 57 - 8, 306, 14, 32});
    checkRect(between.uv, {(56 - 22.0f / 3) / 128, 0, (44.0f / 3) / 128, 1});

    // An empty meter shows nothing of the green arch, and so do a meter run below nothing and
    // one that is not a number.
    for (const f32 shown : {0.0f, -5.0f, std::numeric_limits<f32>::quiet_NaN()}) {
        CAPTURE(shown);
        const LegendsDreamcastTurboArch arch = dreamcastTurbo(shown, kX);
        CHECK(arch.art == "TRBO_GRADNT_GREEN");
        CHECK(arch.area.width == 0);
    }
}

TEST_CASE("the full arch's glow swings twelve a step between nought and 240",
          "[legends][dreamcast-hud]") {
    // FUN_8c0c13e0 steps the glow's transparency byte: an even one up by 12, to the odd 239 once
    // past 240; an odd one down by 12, to nought once it would be under 12.
    std::vector<u8> pulse;
    for (s64 step = 0; step <= 41; ++step) {
        pulse.push_back(dreamcastTurboGlow(step));
    }
    std::vector<u8> expected;
    for (s32 up = 0; up <= 240; up += 12) {
        expected.push_back(static_cast<u8>(up));
    }
    for (s32 down = 239; down >= 23; down -= 12) {
        expected.push_back(static_cast<u8>(down));
    }
    expected.push_back(0);
    expected.push_back(12);
    CHECK(pulse == expected);
    // Forty steps come round to where they started, however far it has run.
    for (s64 step = 0; step < Layout::kGlowPeriod; ++step) {
        CAPTURE(step);
        CHECK(dreamcastTurboGlow(step + (10 * s64{Layout::kGlowPeriod})) ==
              dreamcastTurboGlow(step));
    }
    CHECK(dreamcastTurboGlow(-3) == 0);
}

TEST_CASE("Dreamcast gold starts at X+3, or ends at X+35 while it is narrower than 32",
          "[legends][dreamcast-hud]") {
    // FUN_8c0c0f40 (8c0c1364..8c0c1384): 31 units wide or less, the gold ends at X+35.
    CHECK(dreamcastGoldLeft(9, kX) == kX + 35 - 9);
    CHECK(dreamcastGoldLeft(31, kX) == kX + 4);
    CHECK(dreamcastGoldLeft(0, kX) == kX + 35);
    CHECK(dreamcastGoldLeft(32, kX) == kX + 3);
    CHECK(dreamcastGoldLeft(45, 8) == 11);
}

TEST_CASE("each of Dark Legacy's pickup cards shows as the Dreamcast's card of its kind, or as "
          "its own",
          "[legends][dreamcast-hud]") {
    // FUN_8c0b7560's six cards (8c125004..8c12502c) stand for Dark Legacy's of their kinds; the
    // runestone, legend, crystal and gargoyle cards, which the Dreamcast has none of, stay Dark
    // Legacy's.
    struct Shown {
        std::string_view card;
        LegendsDreamcastCard face;
    };
    constexpr std::array<Shown, 15> kShown{{
        {.card = "GOLD", .face = {.art = "GOLD", .darkLegacy = false}},
        {.card = "JUNK", .face = {.art = "GOLD", .darkLegacy = false}},
        {.card = "COINHUD", .face = {.art = "GOLD", .darkLegacy = false}},
        {.card = "KEY", .face = {.art = "KEY", .darkLegacy = false}},
        {.card = "KEY_RING", .face = {.art = "KEY", .darkLegacy = false}},
        {.card = "MEAT", .face = {.art = "FOOD", .darkLegacy = false}},
        {.card = "FRUIT", .face = {.art = "FOOD", .darkLegacy = false}},
        {.card = "BADMEAT", .face = {.art = "POISN_FOOD", .darkLegacy = false}},
        {.card = "BADFRUIT", .face = {.art = "POISN_FOOD", .darkLegacy = false}},
        {.card = "MAGIC", .face = {.art = "MAGIC", .darkLegacy = false}},
        {.card = "SPECIALS", .face = {.art = "SPECIALS", .darkLegacy = false}},
        {.card = "RUNESTONE", .face = {.art = "RUNESTONE", .darkLegacy = true}},
        {.card = "LEGEND", .face = {.art = "LEGEND", .darkLegacy = true}},
        {.card = "CRYSTAL", .face = {.art = "CRYSTAL", .darkLegacy = true}},
        {.card = "GOLDNICON", .face = {.art = "GOLDNICON", .darkLegacy = true}},
    }};
    for (const Shown& shown : kShown) {
        CAPTURE(shown.card);
        const auto face = dreamcastCard(shown.card);
        REQUIRE(face);
        CHECK(*face == shown.face);
    }
    // A name no pickup puts up shows nothing, the cards' strip and a lower-case name included.
    for (const std::string_view name : {"", "S3", "gold", "BOOK"}) {
        CAPTURE(name);
        CHECK_FALSE(dreamcastCard(name));
    }

    // Every card Dark Legacy's own pickups put up has its face: each kind of item a character
    // takes, and the crystal's.
    const std::array<ItemOffer, 17> offers{{
        {.kind = static_cast<s32>(ItemKind::Gold), .amount = 50},
        {.kind = static_cast<s32>(ItemKind::Gold), .amount = 5},
        {.kind = static_cast<s32>(ItemKind::Keys), .amount = 1},
        {.kind = static_cast<s32>(ItemKind::Keys), .amount = 3},
        {.kind = static_cast<s32>(ItemKind::Food), .amount = 100},
        {.kind = static_cast<s32>(ItemKind::Food), .amount = 20},
        {.kind = static_cast<s32>(ItemKind::Food), .amount = -100},
        {.kind = static_cast<s32>(ItemKind::Food), .amount = -20},
        {.kind = static_cast<s32>(ItemKind::Potion), .amount = 1, .flags = 1},
        {.kind = static_cast<s32>(ItemKind::WeaponPowerup), .amount = 1, .strength = 1.0f},
        {.kind = static_cast<s32>(ItemKind::ArmorPowerup), .amount = 1, .strength = 1.0f},
        {.kind = static_cast<s32>(ItemKind::SpeedPowerup), .amount = 1, .strength = 1.0f},
        {.kind = static_cast<s32>(ItemKind::MagicPowerup), .amount = 1, .strength = 1.0f},
        {.kind = static_cast<s32>(ItemKind::SpecialPowerup), .amount = 1, .strength = 1.0f},
        {.kind = static_cast<s32>(ItemKind::Runestone), .amount = 0},
        {.kind = static_cast<s32>(ItemKind::Legend), .amount = 1},
        {.kind = static_cast<s32>(ItemKind::GargoyleKey), .amount = 0},
    }};
    std::vector<std::string> cards;
    for (const ItemOffer& offer : offers) {
        CAPTURE(offer.kind, offer.amount);
        CharacterSave save;
        save.progress().health = 1;
        const ItemTaking taking = takeItem(save, offer, 1.0f, false);
        REQUIRE_FALSE(taking.card.empty());
        cards.emplace_back(taking.card);
    }
    cards.emplace_back(PickupHud::kCrystalCard);
    for (const std::string& card : cards) {
        CAPTURE(card);
        CHECK(dreamcastCard(card));
    }
    // Between them they put up every card the table names but the secret coins', which the
    // challenge puts up.
    std::ranges::sort(cards);
    const auto repeated = std::ranges::unique(cards);
    cards.erase(repeated.begin(), repeated.end());
    CHECK(cards.size() == kShown.size() - 1);
}

TEST_CASE("WAIT HERE shows four steps in eight while a seat waits with company",
          "[legends][dreamcast-hud]") {
    // FUN_8c0c16e0: while bit 2 of the frame counter is set (*8c287c78 & 4), and only with more
    // than one seat in the game (*8c287bb0 > 1).
    for (s64 step = 0; step < 32; ++step) {
        CAPTURE(step);
        const bool on = step % 8 >= 4;
        CHECK(dreamcastWaitHereShown(true, 2, step) == on);
        CHECK(dreamcastWaitHereShown(true, 4, step) == on);
        CHECK_FALSE(dreamcastWaitHereShown(true, 1, step));
        CHECK_FALSE(dreamcastWaitHereShown(true, 0, step));
        CHECK_FALSE(dreamcastWaitHereShown(false, 4, step));
    }
}

TEST_CASE("a fallen player left alone in the game is asked to return to the tower",
          "[legends][dreamcast-hud]") {
    // FUN_8c0bda40 counts the other seats playing, dying, asked or in the tower (states 1, 11,
    // 12 and 14); with none it asks RETURN / TO TOWER? in place of WAIT / IN TOWER?.
    HudSeat playing;
    playing.box.mode = StatusBoxView::Mode::Status;
    playing.box.active = true;
    playing.turbo = 0.0f;
    HudSeat dying = playing;
    dying.turbo.reset();
    HudSeat inTower = dying;
    inTower.box.inTower = true;
    HudSeat asked = inTower;
    asked.box.towerPrompt = true;
    // One waiting at an exit is in state 7, which is not counted; a player gone from the game, or
    // a seat nobody joined, is in none.
    HudSeat waiting = playing;
    waiting.waitingAtExit = true;
    const PlayerRuntime runtime{};
    HudSeat departed;
    departed.runtime = &runtime;
    departed.departed = true;
    using Seats = std::array<HudSeat, 4>;
    CHECK(dreamcastPromptReturns(Seats{asked, {}, {}, {}}, 0));
    CHECK(dreamcastPromptReturns(Seats{asked, waiting, departed, {}}, 0));
    for (const HudSeat& other : {playing, dying, inTower, asked}) {
        CHECK_FALSE(dreamcastPromptReturns(Seats{asked, {}, other, {}}, 0));
        CHECK_FALSE(dreamcastPromptReturns(Seats{other, waiting, {}, asked}, 3));
    }
    // A player who falls on an exit no longer waits there: it is counted.
    HudSeat fellWaiting = dying;
    fellWaiting.waitingAtExit = true;
    CHECK_FALSE(dreamcastPromptReturns(Seats{asked, fellWaiting, {}, {}}, 0));
}

TEST_CASE("each Dreamcast seat state picks its box", "[legends][dreamcast-hud]") {
    const PlayerRuntime runtime{};
    HudSeat standing;
    standing.box.mode = StatusBoxView::Mode::Status;
    standing.box.active = true;
    standing.box.health = 450;
    standing.runtime = &runtime;
    standing.turbo = 50.0f;
    // A player standing or dying keeps the panel: the death plays out in it.
    HudSeat dying = standing;
    dying.box.health = 0;
    dying.turbo.reset();
    // Fallen to the tower, asked whether to wait there, or gone: the coloured kind-0 box.
    HudSeat inTower = dying;
    inTower.box.inTower = true;
    HudSeat asked = inTower;
    asked.box.towerPrompt = true;
    HudSeat departed;
    departed.runtime = &runtime;
    departed.departed = true;
    CHECK(dreamcastBoxKind(standing) == LegendsDreamcastBoxKind::Panel);
    CHECK(dreamcastBoxKind(dying) == LegendsDreamcastBoxKind::Panel);
    CHECK(dreamcastBoxKind(inTower) == LegendsDreamcastBoxKind::Plain);
    CHECK(dreamcastBoxKind(asked) == LegendsDreamcastBoxKind::Plain);
    CHECK(dreamcastBoxKind(departed) == LegendsDreamcastBoxKind::Plain);
    // A seat nobody joined shows nothing.
    CHECK(dreamcastBoxKind(HudSeat{}) == LegendsDreamcastBoxKind::None);
}

TEST_CASE("the Dreamcast's colours and classes run in Dark Legacy's orders",
          "[legends][dreamcast-hud]") {
    // 8c14bb98's inks are Dark Legacy's player colours, and 8c14bb04's names and the costume
    // banks' folders go yellow, blue, red, green as Dark Legacy's costume colours do.
    constexpr std::array<std::string_view, 4> kNames{"YELO", "BLUE", "RED", "GREEN"};
    for (s32 color = 0; color < kColorCount; ++color) {
        CAPTURE(color);
        const auto at = static_cast<usize>(color);
        CHECK(Layout::kTextColours.at(at) == playerColor(color));
        CHECK(dreamcastPanelColour(color) == kNames.at(at));
        CHECK(Layout::kColourFolders.at(at) == toLowerAscii(colorCode(color)));
    }
    CHECK_FALSE(dreamcastPanelColour(-1));
    CHECK_FALSE(dreamcastPanelColour(kColorCount));
    // 8c124f18's twelve classes are Dark Legacy's first twelve, by the same codes; the rest
    // have no bank of their own.
    for (s32 classIndex = 0; classIndex < 12; ++classIndex) {
        CAPTURE(classIndex);
        CHECK(dreamcastClassCode(classIndex) == classCode(classIndex));
    }
    for (const s32 classIndex :
         {kOgreClass, kUnicornClass, kMedusaClass, kHyenaClass, kSumnerClass, -1, kClassCount}) {
        CAPTURE(classIndex);
        CHECK_FALSE(dreamcastClassCode(classIndex));
    }
}

TEST_CASE("the Dreamcast's five legend places keep their own items and give Dark Legacy's the "
          "rest",
          "[legends][dreamcast-hud]") {
    using Places = std::array<std::optional<s32>, 5>;
    // FUN_8c0c16e0 shows realm n's item in place n - 1 while it is held.
    CHECK(dreamcastQuestPlaces(0) == Places{});
    CHECK(dreamcastQuestPlaces(legendsOf({1, 2, 3, 4, 5})) == Places{1, 2, 3, 4, 5});
    CHECK(dreamcastQuestPlaces(legendsOf({3})) ==
          Places{std::nullopt, std::nullopt, 3, std::nullopt, std::nullopt});
    // Dark Legacy's others, which the Dreamcast has no place for, take the first places whose
    // own item is not held, in realm order: the book, the fire scroll, the lantern, the javelin.
    CHECK(dreamcastQuestPlaces(legendsOf({7})) == Places{7, {}, {}, {}, {}});
    CHECK(dreamcastQuestPlaces(legendsOf({1, 7})) == Places{1, 7, {}, {}, {}});
    CHECK(dreamcastQuestPlaces(legendsOf({5, 11})) == Places{11, {}, {}, {}, 5});
    CHECK(dreamcastQuestPlaces(legendsOf({2, 4, 7, 9, 10, 11})) == Places{7, 2, 9, 4, 10});
    CHECK(dreamcastQuestPlaces(legendsOf({1, 3, 7, 9, 10, 11})) == Places{1, 7, 3, 9, 10});
    // With every place taken, one left over is not shown.
    CHECK(dreamcastQuestPlaces(legendsOf({1, 2, 3, 4, 5, 7, 9, 10, 11})) == Places{1, 2, 3, 4, 5});
    // The realms whose bosses have no legend item take no place.
    CHECK(dreamcastQuestPlaces(legendsOf({0, 6, 8, 12, 13, 14, 15})) == Places{});
}

TEST_CASE("the Dreamcast box's legend items are Dark Legacy's, with its own icons for the five "
          "it has",
          "[legends][dreamcast-hud]") {
    // Every boss's legend item, by its realm, and no other.
    std::vector<s32> realms;
    for (s32 kind = kFirstBossType; kind <= kLastBossType; ++kind) {
        if (const s32 realm = legendRealmOf(kind); realm != 0) {
            realms.push_back(realm);
        }
    }
    std::ranges::sort(realms);
    std::vector<s32> listed;
    listed.reserve(Layout::kLegends.size());
    for (const LegendsDreamcastLegend& legend : Layout::kLegends) {
        listed.push_back(legend.realm);
    }
    CHECK(listed == realms);
    // 8c1255f8: QUEST_ICON%d names the Dreamcast's own five, realms 1 to 5 in their places'
    // order; the rest have none.
    for (usize index = 0; index < Layout::kLegends.size(); ++index) {
        CAPTURE(index);
        const LegendsDreamcastLegend& legend = Layout::kLegends.at(index);
        if (index < Layout::kQuestPlaces.size()) {
            CHECK(legend.realm == static_cast<s32>(index) + 1);
            CHECK(legend.icon == std::format("QUEST_ICON{}", index + 1));
        } else {
            CHECK(legend.icon.empty());
        }
    }
    // Each item's stand-in is the picture of it that Dark Legacy's inventory panel shows while
    // it is held.
    for (const LegendsDreamcastLegend& legend : Layout::kLegends) {
        CAPTURE(legend.realm);
        InventoryContents contents;
        contents.legends = legendsOf({legend.realm});
        InventoryPanel panel;
        panel.open(contents);
        panel.step(InventoryPanel::kEnterTicks, false);
        const std::vector<InventoryPiece> pieces = panel.pieces(0);
        CHECK(std::ranges::any_of(
            pieces, [&](const InventoryPiece& piece) { return piece.texture == legend.picture; }));
    }
}

TEST_CASE("Dreamcast help sits 62 over its anchor, on its box when it has none, and keeps to the "
          "screen",
          "[legends][dreamcast-hud]") {
    using Box = LegendsDreamcastHelpBox;
    // FUN_8c0e3420: (the longest line's characters + 6) x 9 across, (the lines + 3) x 8 down.
    const std::vector<std::string> one{"WATCH OUT"};
    const std::vector<std::string> two{"USE KEY", "TO OPEN DOORS"};
    // Over a projected point its middle stands 54 over its anchor, 8 over the point: 62 over
    // it, Dark Legacy's own rule. Its line runs down its middle, 12 under its top.
    CHECK(Layout::kHelpPointLift + Layout::kHelpOverAnchor == HelpMessages::kAboveHead);
    CHECK(dreamcastHelpBox(one, {300, 250}, true, 1) ==
          Box{.x = 233, .y = 172, .width = 135, .height = 32, .textCentre = 300, .textTop = 184});
    // A seat's message with nothing to stand over stands over its box, its middle 54 over the
    // box's centre 80 up from the bottom.
    CHECK(dreamcastHelpBox(two, {256, 192}, false, 2) ==
          Box{.x = 259, .y = 230, .width = 171, .height = 40, .textCentre = 344, .textTop = 242});
    // Over the outer boxes it keeps to the frame's sides.
    constexpr std::array<s32, 4> kCentres{85, 168, 344, 512 - 171 + 85};
    for (usize seat = 0; seat < Layout::kSeatX.size(); ++seat) {
        CAPTURE(seat);
        const Box box = dreamcastHelpBox(two, {256, 192}, false, static_cast<s32>(seat));
        CHECK(box.y + (box.height / 2) == 250);
        CHECK(box.textCentre == kCentres.at(seat));
    }
    // A projected point wins over the box; a message at a place, with no point, stands over
    // Dark Legacy's middle of the screen.
    CHECK(dreamcastHelpBox(one, {300, 250}, true, 2) == dreamcastHelpBox(one, {300, 250}, true, 1));
    CHECK(dreamcastHelpBox(one, {256, 192}, false, -1) ==
          Box{.x = 189, .y = 114, .width = 135, .height = 32, .textCentre = 256, .textTop = 126});
    // An odd width is halved down, as the original shifts it.
    const std::vector<std::string> letter{"A"};
    CHECK(dreamcastHelpBox(letter, {300, 250}, true, 0) ==
          Box{.x = 269, .y = 172, .width = 63, .height = 32, .textCentre = 300, .textTop = 184});

    // Kept across the frame, its top from 2 down and its bottom over the boxes, 80 up from the
    // frame's bottom, with its lines on it.
    CHECK(Layout::kHelpFloor == HelpMessages::kLowest);
    const Box left = dreamcastHelpBox(one, {10, 250}, true, 0);
    CHECK(left.x == 0);
    CHECK(left.textCentre == 67);
    const Box right = dreamcastHelpBox(one, {500, 250}, true, 0);
    CHECK(right.x == 512 - 135);
    CHECK(right.textCentre == 512 - 135 + 67);
    const Box high = dreamcastHelpBox(one, {300, 40}, true, 0);
    CHECK(high.y == 2);
    CHECK(high.textTop == 14);
    const Box low = dreamcastHelpBox(two, {300, 400}, true, 0);
    CHECK(low.y == 304 - 40);
    CHECK(low.textTop == 304 - 40 + 12);
    // A point off the frame, or no number at all, keeps it there too.
    CHECK(dreamcastHelpBox(one, {-1.0e9f, 1.0e9f}, true, 0) ==
          Box{.x = 0, .y = 272, .width = 135, .height = 32, .textCentre = 67, .textTop = 284});
    constexpr f32 kNan = std::numeric_limits<f32>::quiet_NaN();
    CHECK(dreamcastHelpBox(one, {kNan, kNan}, true, 0) ==
          Box{.x = 0, .y = 2, .width = 135, .height = 32, .textCentre = 67, .textTop = 14});
}

TEST_CASE("each colour's help is written on its own scroll in its own ink, Dark Legacy's",
          "[legends][dreamcast-hud]") {
    // 8c14e7cc and 8c14e7e0: yellow, blue, red and green, then the plain scroll in its brown for
    // a message for nobody; the inks are Dark Legacy's help's, by the same colours.
    constexpr std::array<std::string_view, 4> kScrolls{"YEL_TEXTSCROLL", "BLU_TEXTSCROLL",
                                                       "RED_TEXTSCROLL", "GRE_TEXTSCROLL"};
    for (s32 color = 0; color < kColorCount; ++color) {
        CAPTURE(color);
        const usize scroll = dreamcastHelpScroll(color);
        CHECK(Layout::kHelpScrolls.at(scroll) == kScrolls.at(static_cast<usize>(color)));
        CHECK(Layout::kHelpInks.at(scroll) == HelpMessages::inkOf(color));
    }
    for (const s32 color : {-1, kColorCount, 7}) {
        CAPTURE(color);
        const usize scroll = dreamcastHelpScroll(color);
        CHECK(Layout::kHelpScrolls.at(scroll) == "TEXTSCROLL");
        CHECK(Layout::kHelpInks.at(scroll) == ScrollBox::kTextColor);
        CHECK(Layout::kHelpInks.at(scroll) == Layout::kWaitHereInk);
    }
}

TEST_CASE("the Dreamcast thermometer draws the linear column from Dark Legacy's fill",
          "[legends][dreamcast-hud]") {
    // FUN_8c09b4e0: the column round(73(1 - f)) + 27 tall, its top 102 less that, the picture
    // from (101 - 73(1 - f))/128 down: straight with the closeness 1 - f, of which Dark Legacy's
    // fill is the square.
    for (const f32 closeness : {0.0f, 0.25f, 0.5f, 0.8f, 1.0f}) {
        CAPTURE(closeness);
        const RuneMeter::Column column = dreamcastThermometerColumn(closeness * closeness, 128);
        const RuneMeter::Column expected = RuneMeter::column(closeness, 128);
        checkRect(column.area, expected.area);
        checkRect(column.uv, expected.uv);
        const f32 raised = std::round(73 * closeness);
        checkRect(column.area, {392, 102 - raised, 128, raised + 27});
        const f32 from = (101 - (73 * closeness)) / 128;
        checkRect(column.uv, {0, from, 1, 1 - from});
    }
    // At Dark Legacy's quarter fill the column stands half its way up, where Dark Legacy's own
    // stands a quarter of its way; it stands at the base's x.
    checkRect(dreamcastThermometerColumn(0.25f, 128).area, {392, 65, 128, 64});
    checkRect(RuneMeter::column(0.25f, 128).area, {392, 84, 128, 45});
    CHECK(dreamcastThermometerColumn(0.25f, 64).area.x == Layout::kThermometerLeft);
    CHECK(dreamcastThermometerColumn(0.25f, 64).area.width == 64);
    // A fill below nothing, past full, or no number at all keeps to the column's ends.
    checkRect(dreamcastThermometerColumn(-1, 128).area, {392, 102, 128, 27});
    checkRect(dreamcastThermometerColumn(std::numeric_limits<f32>::quiet_NaN(), 128).area,
              {392, 102, 128, 27});
    checkRect(dreamcastThermometerColumn(4, 128).area, {392, 29, 128, 100});
}
