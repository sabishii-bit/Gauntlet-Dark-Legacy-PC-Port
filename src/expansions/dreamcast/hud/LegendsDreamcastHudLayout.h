#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/hud/HudView.h"

namespace gdl::game {

/** A place in a Dreamcast status box: canvas units from its seat's column, and from the canvas's
 * top to the top edge of what stands there. */
struct LegendsDreamcastPoint {
    s32 x = 0;
    s32 y = 0;
};

/** One zone of the turbo arch: its picture, where on the meter's 100 it starts and how far it
 * runs, and the arch's half-width, in texels, at its start and how far it opens across it. */
struct LegendsDreamcastTurboZone {
    std::string_view art;
    f32 from = 0;
    f32 length = 0;
    f32 start = 0;
    f32 span = 0;
};

/**
 * Where 1ST_READ.BIN's HUD puts the parts of a status box. Its frame is 512x384, the canvas's
 * own, so every number is in canvas units. Each seat keeps its column X whatever the size of
 * the party, and its box's art stands 8 left of it; a place's x is from X, and its y is the top
 * edge of what stands there, measured from the top as the blits' negative y measures it.
 */
struct LegendsDreamcastHudLayout {
    // 8c14bbb8 and 8c14bbc0: each seat's column and its box's centre.
    static constexpr std::array<s32, 4> kSeatX{8, 120, 296, 408};
    static constexpr std::array<s32, 4> kSeatCentre{56, 168, 344, 456};
    /** 8c14bb98: each costume colour's ink, yellow, blue, red and green. */
    static constexpr std::array<Color, 4> kTextColours{
        Color::rgba(0xFF, 0xFF, 0x80), Color::rgba(0x87, 0xCE, 0xEB), Color::rgba(0xFF, 0xC0, 0xE0),
        Color::rgba(0x80, 0xFF, 0x80)};
    /** 8c14bb04: each costume colour's name in the boxes' art: BTMBK_, S3_W and S4_W. */
    static constexpr std::array<std::string_view, 4> kPanelColours{"YELO", "BLUE", "RED", "GREEN"};
    /** The extraction's folder of each costume colour's bank, players/<class>/<colour>. */
    static constexpr std::array<std::string_view, 4> kColourFolders{"yel", "blu", "red", "gre"};
    /** 8c124f18: the classes with costume banks of their own, which hold their panels and their
     * banners, in the order of Dark Legacy's first twelve. */
    static constexpr std::array<std::string_view, 12> kClassCodes{
        "WAR", "VAL", "WIZ", "ARC", "DWF", "KNI", "SOR", "JES", "MIN", "FAL", "JAC", "TIG"};

    // FUN_8c0ba060 and FUN_8c0c08a0: a box's art, from 8 left of its column. A player's box is
    // the stone strip over the costume's panel, with the class's banner on it; a kind-0 box,
    // the coloured strip over the coloured panel alone. The panels are 128 rows tall, of which
    // the screen shows the top 64.
    static constexpr s32 kArtLeft = -8;
    static constexpr s32 kStripTop = 306;      ///< BK_RUNE_STONE_02
    static constexpr s32 kPlainStripTop = 304; ///< S3_W<colour>
    static constexpr s32 kPanelTop = 320;      ///< BTMBK_<colour>, or S4_W<colour>
    static constexpr s32 kPanelRows = 64;
    static constexpr s32 kBannerTop = 338; ///< <CLS>_NAME
    static constexpr LegendsDreamcastPoint kLevelLabel{34, 323};
    static constexpr LegendsDreamcastPoint kGoldLabel{5, 357};
    static constexpr LegendsDreamcastPoint kHealthLabel{53, 357};

    // FUN_8c0c0f40 and FUN_8c0c16e0: the level in font 1, white, centred on the box; gold and
    // health in font 4, in the seat's ink, gold from kGoldLeft unless it is narrower than
    // kGoldNarrow, when it ends at kGoldRight, and health ending at kHealthRight.
    static constexpr s32 kLevelTop = 330;
    static constexpr s32 kNumbersTop = 365;
    static constexpr s32 kGoldLeft = 3;
    static constexpr s32 kGoldRight = 35;
    static constexpr s32 kGoldNarrow = 32;
    static constexpr s32 kHealthRight = 92;
    static constexpr s32 kMostGold = 99999;
    static constexpr s32 kMostHealth = 9999;

    // FUN_8c0c16e0: the keys and the potion thrown next, each icon with its count in font 4 in
    // the seat's ink, only while any are carried.
    static constexpr LegendsDreamcastPoint kKeyIcon{1, 323};
    static constexpr LegendsDreamcastPoint kKeyCount{18, 323};
    static constexpr LegendsDreamcastPoint kPotionIcon{68, 323};
    static constexpr LegendsDreamcastPoint kPotionCount{82, 323};

    /** FUN_8c0c16e0: "IN TOWER", font 1, in the seat's ink, centred on the box. */
    static constexpr s32 kInTowerTop = 344;
    /** The port's: a class with no banner is named in font32 at half its height, which fills
     * the banner's 16 rows. */
    static constexpr f32 kBannerTextScale = 0.5f;

    // FUN_8c0c13e0 (pool 8c0c14e0..8c0c1508, 8c0c16bc..8c0c16cc): the meter's arch, a 128x32
    // picture over the box from kTurboTop. Below 0.3, 0.6 and 0.9999 of the meter it is green,
    // yellow or red, cropped about its texel 56 to a half-width that opens across the zone;
    // from 0.9999 it is TRBO_FULL whole, with TURBO_GLOW pulsing over it.
    static constexpr std::array<LegendsDreamcastTurboZone, 3> kTurboZones{{
        {.art = "TRBO_GRADNT_GREEN", .from = 0, .length = 30, .start = 0, .span = 22},
        {.art = "TRBO_GRADNT_YELO", .from = 30, .length = 30, .start = 22, .span = 18},
        {.art = "TRBO_GRADNT_RED", .from = 60, .length = 40, .start = 40, .span = 12},
    }};
    static constexpr f32 kTurboFull = 99.99f;
    static constexpr f32 kTurboMost = 100;
    static constexpr std::string_view kTurboFullArt = "TRBO_FULL";
    static constexpr std::string_view kTurboGlowArt = "TURBO_GLOW";
    static constexpr s32 kTurboTop = 306;
    static constexpr f32 kTurboWidth = 128;
    static constexpr f32 kTurboRows = 32;
    static constexpr f32 kTurboCentre = 56;
    /** The cropped arch's left edge, truncated to a whole unit, is drawn one further right. */
    static constexpr s32 kTurboLeftShift = 1;
    /** The glow's transparency: an even one climbs by kGlowStep, turning to the odd kGlowTurn
     * once past kGlowMost; an odd one falls by kGlowStep, starting over at nought once it
     * reaches kGlowLeast or under. From nought that comes round every kGlowPeriod steps. */
    static constexpr s32 kGlowStep = 12;
    static constexpr s32 kGlowMost = 240;
    static constexpr s32 kGlowTurn = 239;
    static constexpr s32 kGlowLeast = 11;
    static constexpr s32 kGlowPeriod = 40;
};

/** Seat `seat`'s column, and its box's centre. */
s32 dreamcastSeatX(usize seat);
s32 dreamcastSeatCentre(usize seat);

/** The turbo arch a meter showing `shown` of 100 puts over the box in column `seatX`: its
 * picture, where it stands and the part of the picture across it. */
struct LegendsDreamcastTurboArch {
    std::string_view art;
    Rect area{};
    Rect uv{};
    bool full = false; ///< the whole arch, which glows
};

/** The arch FUN_8c0c13e0 draws for `shown`: the zone's picture cropped to its half-width about
 * its texel 56, sampled across that half-width either side, its left edge and width truncated
 * to whole units as the blit keeps them; the full picture from 0.9999 of the meter. */
LegendsDreamcastTurboArch dreamcastTurbo(f32 shown, s32 seatX);

/** The full arch's glow's transparency after `steps` steps of its pulse from nought: up by
 * twelve to 240, down by twelve from 239, nought again after 40. */
u8 dreamcastTurboGlow(s64 steps);

/** Where gold `width` wide starts in the box in column `seatX` (FUN_8c0c0f40). */
s32 dreamcastGoldLeft(s32 width, s32 seatX);

/** Costume colour `color`'s name in the boxes' art; none for a colour past the four. */
std::optional<std::string_view> dreamcastPanelColour(s32 color);

/** Class `classIndex`'s code in the Dreamcast's costume banks and banners; none for a class it
 * has no bank of. */
std::optional<std::string_view> dreamcastClassCode(s32 classIndex);

/** Which box a seat's column shows: FUN_8c0c08a0's kinds. */
enum class LegendsDreamcastBoxKind : u8 {
    None,  ///< nothing: a seat nobody joined, all of whose art starts hidden (FUN_8c0ba060)
    Panel, ///< kind 1: the strip, the costume's panel and banner, and what the player has
    Plain, ///< kind 0: the coloured strip and panel alone
};

/** The box `seat` shows: a player standing or dying, the panel; one fallen to the tower, asked
 * to wait there, or gone from the game, kind 0 (FUN_8c0bbf80, FUN_8c0bc040 and FUN_8c0bbee0 each
 * rebuild the box so, and nothing hides a gone player's); nobody, none. */
LegendsDreamcastBoxKind dreamcastBoxKind(const HudSeat& seat);

} // namespace gdl::game
