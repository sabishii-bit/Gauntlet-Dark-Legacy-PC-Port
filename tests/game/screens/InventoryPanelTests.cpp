#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/screens/InventoryPanel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

InventoryContents contents() {
    InventoryContents result;
    result.gargoylePieces = {3, 20, 40};
    result.crystals = {5, 100, 0, 7, 175, 1, 2, 250};
    result.legends = static_cast<u16>((1U << 2) | (1U << 7));
    result.runes = static_cast<u16>((1U << 0) | (1U << 1) | (1U << 8) | (1U << 9));
    return result;
}

const InventoryPiece* piece(const std::vector<InventoryPiece>& pieces, std::string_view name) {
    const auto found = std::ranges::find(pieces, name, &InventoryPiece::texture);
    return found == pieces.end() ? nullptr : &*found;
}

TEST_CASE("inventory panel flies in over 120 ticks, idles and leaves over 15 on accept",
          "[inventory-panel]") {
    // GUNE5D draw_inventory_panel (8006C644) and animate_panel_piece (8006CCB8):
    // lbl_803473E4 120, lbl_803473E8 15, the spiral of lbl_8011D568/658 at 60 out.
    InventoryPanel panel;
    REQUIRE(panel.phase() == InventoryPanel::Phase::Closed);
    REQUIRE(panel.pieces(0).empty());
    REQUIRE_FALSE(panel.step(1, true));
    panel.open(contents());
    REQUIRE(panel.phase() == InventoryPanel::Phase::Entering);
    REQUIRE(panel.showsPrompt());
    {
        // At the first tick the window starts sixty pixels out along the table's first
        // angle, twice its size and clear; the counts are clear too.
        const auto pieces = panel.pieces(128);
        const auto* window = piece(pieces, "WINDOW_EMPTY");
        REQUIRE(window != nullptr);
        CHECK(window->x == 188);
        CHECK(window->y == 0);
        CHECK(window->size == 2.0f);
        CHECK(window->opacity == 0);
        REQUIRE_FALSE(panel.counts(128).empty());
        CHECK(panel.counts(128)[0].opacity == 0);
    }
    REQUIRE_FALSE(panel.step(60, false));
    {
        const auto pieces = panel.pieces(0);
        const auto* fangs = piece(pieces, "FANGS");
        REQUIRE(fangs != nullptr);
        CHECK(fangs->x == 59);
        CHECK(fangs->y == 145);
        CHECK(fangs->size == 1.75f);
        CHECK(fangs->opacity == 64);
        CHECK(panel.counts(0)[0].opacity == 128);
    }
    REQUIRE_FALSE(panel.step(59, false));
    REQUIRE(panel.phase() == InventoryPanel::Phase::Entering);
    REQUIRE_FALSE(panel.step(1, false));
    REQUIRE(panel.phase() == InventoryPanel::Phase::Idle);
    {
        const auto pieces = panel.pieces(128);
        const auto* window = piece(pieces, "WINDOW_EMPTY");
        REQUIRE(window != nullptr);
        CHECK(window->x == 128);
        CHECK(window->y == 0);
        CHECK(window->size == 1.0f);
        CHECK(window->opacity == 255);
        const auto* fangs = piece(pieces, "FANGS");
        REQUIRE(fangs != nullptr);
        CHECK(fangs->x == 184);
        CHECK(fangs->y == 116);
    }
    REQUIRE(panel.step(0, true));
    REQUIRE(panel.phase() == InventoryPanel::Phase::Leaving);
    REQUIRE_FALSE(panel.showsPrompt());
    REQUIRE_FALSE(panel.step(0, true)); // only a fresh accept, once
    {
        const auto pieces = panel.pieces(128);
        const auto* window = piece(pieces, "WINDOW_EMPTY");
        REQUIRE(window != nullptr);
        CHECK(window->x == 128);
        CHECK(window->opacity == 255);
    }
    REQUIRE_FALSE(panel.step(3, false));
    {
        const auto pieces = panel.pieces(256);
        const auto* fangs = piece(pieces, "FANGS");
        REQUIRE(fangs != nullptr);
        CHECK(fangs->x == 296);
        CHECK(fangs->y == -12);
        CHECK(fangs->size == 2.0f);
        CHECK(fangs->opacity == 204);
        CHECK(panel.counts(256)[0].opacity == 204);
    }
    REQUIRE_FALSE(panel.step(11, false));
    REQUIRE(panel.phase() == InventoryPanel::Phase::Leaving);
    {
        const auto pieces = panel.pieces(128);
        const auto* window = piece(pieces, "WINDOW_EMPTY");
        REQUIRE(window != nullptr);
        CHECK(window->x == -469);
        CHECK(window->y == -1680);
        CHECK(window->size == 5.666666507720947f);
        CHECK(window->opacity == 17);
    }
    REQUIRE_FALSE(panel.step(1, false));
    REQUIRE(panel.done());
    REQUIRE(panel.pieces(0).empty());
    REQUIRE(panel.counts(0).empty());
    REQUIRE_FALSE(panel.step(1, true));
    REQUIRE(panel.done());
}

TEST_CASE("inventory panel accepts while still flying in", "[inventory-panel]") {
    // shop.c 507: state 10 takes the press whatever the panel's mode.
    InventoryPanel panel;
    panel.open(contents());
    REQUIRE(panel.step(10, true));
    REQUIRE(panel.phase() == InventoryPanel::Phase::Leaving);
}

TEST_CASE("inventory panel counts the pieces and crystals against what the tower wants",
          "[inventory-panel]") {
    // print_n_of_m (8006D18C): n clamped to m, written at the row's offset (30, 2) at 0.5
    // for the gargoyle pieces and (28, 2) at 0.35 for the crystals; lbl_80124C70's maxima.
    InventoryPanel panel;
    panel.open(contents());
    panel.step(InventoryPanel::kEnterTicks, false);
    const auto counts = panel.counts(128);
    REQUIRE(counts.size() == 11);
    struct Expected {
        s32 x;
        s32 y;
        f32 scale;
        s32 n;
        s32 m;
    };
    constexpr std::array<Expected, 11> kExpected{{
        {214, 118, 0.5f, 3, 12},
        {214, 142, 0.5f, 20, 20},
        {214, 166, 0.5f, 28, 28},
        {162, 202, 0.35f, 5, 15},
        {162, 218, 0.35f, 100, 100},
        {162, 234, 0.35f, 0, 125},
        {162, 250, 0.35f, 7, 150},
        {220, 202, 0.35f, 175, 175},
        {220, 218, 0.35f, 1, 200},
        {220, 234, 0.35f, 2, 225},
        {220, 250, 0.35f, 250, 250},
    }};
    for (usize i = 0; i < kExpected.size(); ++i) {
        CAPTURE(i);
        CHECK(counts[i].x == kExpected[i].x);
        CHECK(counts[i].y == kExpected[i].y);
        CHECK(counts[i].scale == kExpected[i].scale);
        CHECK(counts[i].n == kExpected[i].n);
        CHECK(counts[i].m == kExpected[i].m);
        CHECK(counts[i].opacity == 255);
    }
}

TEST_CASE("inventory panel shows the legend items held, the empty pictures otherwise, and the "
          "glass of the runestones held",
          "[inventory-panel]") {
    // draw_inventory_panel: towerGetRuneNearStat picks `<name>_empty` (lbl_803473EC) for a
    // realm's item not held; PlayerHasRune (800A2568), not PlayerHasShard, gates the pieces.
    InventoryPanel panel;
    panel.open(contents());
    panel.step(InventoryPanel::kEnterTicks, false);
    const auto pieces = panel.pieces(0);
    REQUIRE(pieces.size() == 12 + 9 + 2);
    CHECK(pieces[0].texture == "WINDOW_EMPTY");
    const auto* axe = piece(pieces, "ICE_AX");
    REQUIRE(axe != nullptr);
    CHECK(axe->x == 76);
    CHECK(axe->y == 32);
    const auto* book = piece(pieces, "BOOK");
    REQUIRE(book != nullptr);
    CHECK(book->x == 54);
    CHECK(book->y == 32);
    CHECK(piece(pieces, "ICE_AX_EMPTY") == nullptr);
    CHECK(piece(pieces, "SCIMITAR") == nullptr);
    for (const auto* empty : {"SCIMITAR_EMPTY", "LAMP_EMPTY", "BILLOWS_EMPTY", "SOUL_SAVIOR_EMPTY",
                              "FIRE_SCROLL_EMPTY", "LANTERN_EMPTY", "JAVILIN_EMPTY"}) {
        CAPTURE(empty);
        CHECK(piece(pieces, empty) != nullptr);
    }
    const auto* scimitar = piece(pieces, "SCIMITAR_EMPTY");
    CHECK(scimitar->x == 96);
    CHECK(scimitar->y == 32);
    const auto* lich = piece(pieces, "LITCH_PIECE");
    REQUIRE(lich != nullptr);
    CHECK(lich->x == 0);
    CHECK(lich->y == 86);
    const auto* wraith = piece(pieces, "WRAITH_PIECE");
    REQUIRE(wraith != nullptr);
    CHECK(wraith->y == 63);
    CHECK(piece(pieces, "DRAGON_PIECE") == nullptr);
    CHECK(piece(pieces, "GENIE_PIECE") == nullptr);
}

TEST_CASE("inventory contents come from the character's progress", "[inventory-panel]") {
    ClassProgress progress;
    progress.relics.gargoylePieces = {1, 2, 3};
    progress.crystals[0] = 99;
    progress.crystals[1] = 15;
    progress.crystals[8] = 4;
    progress.crystals[9] = 7;
    REQUIRE(progress.relics.addLegend(3));
    REQUIRE(progress.relics.addRune(2));
    const auto taken = InventoryContents::of(progress);
    CHECK(taken.gargoylePieces == std::array<s32, 3>{1, 2, 3});
    CHECK(taken.crystals == std::array<s32, 8>{15, 0, 0, 0, 0, 0, 0, 4});
    CHECK(taken.legends == (1U << 3));
    CHECK(taken.runes == (1U << 2));
}
} // namespace
