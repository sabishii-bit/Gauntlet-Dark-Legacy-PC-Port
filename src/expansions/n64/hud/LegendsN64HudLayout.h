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
 * Where game.bin's HUD puts the parts of its status panels in the 416x220 frame. A point is
 * relative to its panel's top left, (kSlotX[seat], kStripTop); a picture's is its centre, as
 * e005a154's records place them, and a line's its left end and top.
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
    static constexpr usize kWordLength = 10; ///< "%.10s"

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
};

} // namespace gdl::game
