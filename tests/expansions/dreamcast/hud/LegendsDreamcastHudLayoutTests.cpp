#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Strings.h"
#include "engine/core/Types.h"

#include "RecordedQuads.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"
#include "game/bosses/LegendItems.h"
#include "game/characters/ClassData.h"
#include "game/hud/HudView.h"
#include "game/hud/StatusBox.h"
#include "game/level/BossKinds.h"
#include "game/party/PlayerRuntime.h"
#include "game/shop/InventoryPanel.h"

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
