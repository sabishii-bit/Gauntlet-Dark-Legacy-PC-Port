#pragma once

#include <array>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "expansions/n64/hud/LegendsN64Frame.h"

namespace gdl::game {

/** A point of the N64's HUD frame, in its units. */
struct LegendsN64Point {
    s32 x = 0;
    s32 y = 0;
};

/** Frame columns [left, right) of one of the turbo meter's lights, the first of them showing
 * TURBOCLR's column `s` of the mirrored tile, in the green, yellow or red `third`. */
struct LegendsN64TurboSpan {
    usize third = 0;
    s32 left = 0;
    s32 right = 0;
    s32 s = 0;
};

/**
 * Where game.bin's HUD puts its parts in the 416x220 frame: the status panels with their pages
 * and carousel, the hourglass, the advice scrolls and the boss meters. A panel's point is
 * relative to its panel's top left, (kSlotX[seat], kStripTop); a picture's is its centre, as
 * e005a154's records place them, and a line's its left end and top. The hourglass's, the
 * scrolls' and the meters' are the frame's own; the port's ticker stands in canvas units.
 */
struct LegendsN64HudLayout {
    // FUN_e0056630 and FUN_e0057b90: four 104x50 panels along the frame's bottom, one a seat,
    // whatever the size of the party.
    static constexpr std::array<s32, 4> kSlotX{0, 104, 208, 312};
    static constexpr s32 kPanelWidth = 104;
    static constexpr s32 kPanelHeight = 50;
    static constexpr s32 kStripTop = static_cast<s32>(LegendsN64Frame::kHeight) - kPanelHeight;

    /** e005a2b0: each costume colour's bright shade, yellow, blue, red and green; a player's
     * panel is lit in it. */
    static constexpr std::array<Color, 4> kBright{Color::rgba(219, 203, 5), Color::rgba(0, 0, 220),
                                                  Color::rgba(180, 0, 0), Color::rgba(0, 200, 25)};
    /** e005a2d0: the dark shades, which an empty panel takes. */
    static constexpr std::array<Color, 4> kDark{Color::rgba(110, 102, 0), Color::rgba(0, 0, 110),
                                                Color::rgba(110, 0, 0), Color::rgba(0, 100, 12)};

    // FUN_e0057f50: the portrait at its top left, lit in the bright shade, its shine over it.
    static constexpr LegendsN64Point kPortrait{4, 10};
    static constexpr LegendsN64Point kPortraitClip{35, 33}; ///< its clip; WIZ_P1 alone is taller
    static constexpr u8 kShineAlpha = 0x7f;

    // e005a154 and FUN_e0056acc: the pictures and their transparency.
    static constexpr LegendsN64Point kKey{13, 16};
    static constexpr LegendsN64Point kPotion{89, 16};
    static constexpr LegendsN64Point kHeart{36, 13};
    static constexpr LegendsN64Point kLeftArrow{12, 44};
    static constexpr LegendsN64Point kRightArrow{92, 44}; ///< ARROW turned about
    static constexpr u8 kHeartShort = 0x78;   ///< the heart's transparency below full health
    static constexpr u8 kArrowResting = 0x80; ///< an arrow's transparency while no step lights it

    // e0059ffc and e005a0f4: the numbers and words, all white.
    static constexpr LegendsN64Point kHealth{41, 9};
    static constexpr s32 kHealthDigits = 4; ///< "%4d"
    static constexpr s32 kMostHealthShown = 9999;
    static constexpr LegendsN64Point kKeyCount{20, 10};
    static constexpr LegendsN64Point kPotionCount{78, 10};
    static constexpr LegendsN64Point kName{25, 30};
    static constexpr LegendsN64Point kClass{20, 40};
    static constexpr usize kWordLength = 10;           ///< "%.10s"
    static constexpr LegendsN64Point kStatLine{8, 28}; ///< the stats page's line, for the name
    static constexpr LegendsN64Point kCount{64, 40};   ///< what the selection has left, "%4d"
    static constexpr s32 kCountDigits = 4;

    // e005a154's records 8 to 10 and FUN_e0059e00: the carousel, its selection between the one
    // before it and the one after, smaller; the quest page lists its entries the same way.
    static constexpr LegendsN64Point kPrevious{28, 28};
    static constexpr LegendsN64Point kSelected{52, 32};
    static constexpr LegendsN64Point kNext{77, 28};
    static constexpr f32 kNeighbourScale = 0.6f;
    /** The HUD bank's item icons are this square; a picture borrowed from Dark Legacy is fitted
     * into it. */
    static constexpr f32 kIconSize = 28;
    static constexpr u8 kSwitchedOff = 0x80; ///< a powerup's icon's transparency while it is off
    /** FUN_e0056acc: the key's and the bottle's transparency while the carousel or the quest
     * page shows. */
    static constexpr u8 kCarriedDimmed = 0x78;

    /** The port's: the N64 has no fallen prompt, so its two lines stand centred in the empty
     * panel at these tops. */
    static constexpr s32 kPromptCentre = 52;
    static constexpr std::array<s32, 2> kPromptLineTops{16, 28};

    /** FUN_e0059368, FUN_e0059524 and FUN_e0059834: the turbo meter's lights, TURBOCLR's 15 rows
     * in green, yellow and red. */
    static constexpr s32 kTurboRows = 15;
    static constexpr std::array<Color, 3> kTurboColors{Color::rgba(0x33, 0xfd, 0x08),
                                                       Color::rgba(0xf5, 0xff, 0x16),
                                                       Color::rgba(0xff, 0x12, 0x0a)};
    static constexpr std::array<LegendsN64TurboSpan, 5> kTurboSpans{{
        {.third = 0, .left = 38, .right = 66, .s = 38},
        {.third = 1, .left = 15, .right = 37, .s = 15},
        {.third = 1, .left = 67, .right = 88, .s = 67},
        {.third = 2, .left = 1, .right = 15, .s = 0},
        {.third = 2, .left = 89, .right = 103, .s = 89},
    }};

    /** The port's: the N64 has no ticker. Dark Legacy's counts stack up from this canvas row,
     * clear of the panels' top (300.28); no label rises over the panels to push them. */
    static constexpr s32 kTickerRestTop = 282;

    /** The port's plaque for an exit of Dark Legacy's, lettered as the N64's LNAME pictures
     * are: the realm's name in gold from row kPlaqueRealmTop, the level's in white from
     * kPlaqueTitleTop, each centred on the panel and shrunk to kPlaqueTextWidth when wider. */
    static constexpr s32 kPlaqueRealmTop = 9;
    static constexpr s32 kPlaqueTitleTop = 22;
    static constexpr s32 kPlaqueTextWidth = 96;
    static constexpr Color kPlaqueRealmInk = Color::rgba(255, 231, 132);

    // FUN_e0069ee0: the hourglass, TIMER_P0's glass with TIMER_P1's sand drawn from the same
    // corner, cropped to the rows of sand each bulb holds, and SAND_ANIM's grains between them.
    // f, the share of the time gone, drains the top bulb from its top and fills the bottom one
    // up from its foot, while the stream of grains shortens to meet it.
    static constexpr LegendsN64Point kTimer{15, 25}; ///< TIMER_P0's and TIMER_P1's top left
    static constexpr s32 kUpperSandRow = 11;         ///< TIMER_P1's first row of the full top bulb
    static constexpr s32 kLowerSandRow = 51;         ///< the row past the full bottom bulb's last
    static constexpr s32 kSandRows = 20;             ///< the rows of sand a full bulb holds
    static constexpr LegendsN64Point kSandStream{54, 56}; ///< SAND_ANIM's top left, 20 rows long
    static constexpr u8 kTimerTransparency = 0x40; ///< the glass's and the sand's; grains opaque
    /** SAND_ANIM00..04. Assumed: the grains change with Dark Legacy's falling counter, as
     * neither the N64's rate nor the Dreamcast's was measured. */
    static constexpr usize kSandFrames = 5;

    // FUN_e00717a0 and FUN_e0071a10: an advice scroll as wide as the first of e00ac0e4's widths
    // whose wrap, e00ac0cc's, takes the widest line within kHelpLinesMost lines, else the last;
    // its text kHelpTextTop below its top and kHelpInset in for every kHelpInsetSpan across.
    static constexpr std::array<s32, 6> kHelpWidths{70, 105, 140, 175, 210, 350};
    static constexpr std::array<s32, 6> kHelpWraps{40, 60, 80, 100, 120, 240};
    static constexpr usize kHelpLinesMost = 3;
    static constexpr s32 kHelpMargin = 16; ///< the scroll is this much taller than its text
    static constexpr s32 kHelpTextTop = 6;
    static constexpr s32 kHelpInset = 12;
    static constexpr s32 kHelpInsetSpan = 70;
    static constexpr s32 kHelpTop = 1; ///< the highest a scroll stands; it stays above the panels

    // Boss.bin FUN_e08887b8 and FUN_e0888a7c: a meter's strips at the frame's top, each
    // kMeterStripWidth across, with insets its type gives for strips of 256.
    static constexpr s32 kMeterStripWidth = 200;
    static constexpr s32 kMeterTop = 1;
    /** The port's: a meter of art drawn for the canvas, a Dark Legacy boss's, hangs at the
     * canvas row of the frame's row 1 (10.29). */
    static constexpr s32 kForeignMeterTop = 10;
    /** FUN_e08887b8: each bind sets one step for every meter, a hundredth of its maximum.
     * Assumed: the step is taken once a painter step, as the N64's boss update rate was not
     * measured. */
    static constexpr s32 kMeterSteps = 100;
};

} // namespace gdl::game
