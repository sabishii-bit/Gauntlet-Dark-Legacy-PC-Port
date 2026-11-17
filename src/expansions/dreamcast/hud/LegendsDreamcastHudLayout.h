#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/ui/TextPainter.h"

#include "game/hud/HudView.h"
#include "game/hud/RuneMeter.h"
#include "game/hud/StatusBox.h"

namespace gdl::game {

/** A place in a Dreamcast status box: canvas units from its seat's column, and from the canvas's
 * top to the top edge of what stands there. */
struct LegendsDreamcastPoint {
    s32 x = 0;
    s32 y = 0;
};

/** A legend item by the realm whose boss it is for: its icon in the Dreamcast's powerups bank,
 * for the five the Dreamcast has, and Dark Legacy's SELECT picture of it. */
struct LegendsDreamcastLegend {
    s32 realm = 0;
    std::string_view icon;    ///< QUEST_ICON<n>; empty for an item the Dreamcast has none of
    std::string_view picture; ///< SELECT's, as the inventory panel shows the item
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

/** A pickup card of Dark Legacy's and the Dreamcast's card of the same kind. */
struct LegendsDreamcastCardKind {
    std::string_view card; ///< Dark Legacy's STATIC card, as its pickups name it
    std::string_view face; ///< the Dreamcast's static set's card; empty for none
};

/**
 * Where 1ST_READ.BIN's HUD puts the parts of a status box. Its frame is 512x384, the canvas's
 * own, so every number is in canvas units. Each seat keeps its column X whatever the size of
 * the party, and its box's art stands 8 left of it; a place's x is from X, and its y is the top
 * edge of what stands there, measured from the top as the blits' negative y measures it.
 */
struct LegendsDreamcastHudLayout {
    /** The Dreamcast's frame, the canvas's own. */
    static constexpr s32 kFrameWidth = 512;
    static constexpr s32 kFrameHeight = 384;
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

    // The words the boxes write, by their ids in the string table: 1ST_READ.BIN's own, IN TOWER
    // at 8c1256b4 (Dark Legacy's words too), WAIT HERE... at 8c1256c0, and the prompt's WAIT,
    // IN TOWER?, YES and NO at 8c1254cc, 8c1254d4, 8c1254f4 and 8c1254f8.
    static constexpr std::string_view kInTowerText = "hud.inTower";
    static constexpr std::string_view kWaitHereText = "legends.dreamcast.hud.waitHere";
    static constexpr std::string_view kWaitText = "legends.dreamcast.hud.wait";
    static constexpr std::string_view kInTowerQuestionText =
        "legends.dreamcast.hud.inTowerQuestion";
    static constexpr std::string_view kYesText = "legends.dreamcast.hud.yes";
    static constexpr std::string_view kNoText = "legends.dreamcast.hud.no";

    // FUN_8c0bda40 (8c0bf416..8c0bf5b4): a fallen player's box asks whether to wait in the
    // tower, each line in font 1 centred on the box: WAIT and IN TOWER? in the seat's ink, then
    // YES and NO. The choice its cursor is on is red (8c14bcc8[1]) and the other is in the
    // seat's ink, which 8c0bf42e stores over the table's white before every ask; the cursor
    // starts on YES (FUN_8c0bc040).
    static constexpr s32 kPromptWaitTop = 336;
    static constexpr s32 kPromptQuestionTop = 344;
    static constexpr s32 kPromptYesTop = 356;
    static constexpr s32 kPromptNoTop = 368;
    static constexpr Color kPromptChosenInk = Color::rgba(0xFF, 0x00, 0x00);

    // FUN_8c0c16e0 (8c0c19ee..8c0c1a8e): a player waiting at an exit is told so over its box,
    // TEXTSCROLL squeezed into 112x16 from 8 left of the column at 324, and WAIT HERE... over it
    // in font 1, in the scroll's brown, from X+8 at 328. The scroll shows while bit 2 of the
    // dispatcher's frame counter is set (*8c287c78 & 4), four frames in eight, and only with
    // more than one seat in the game (*8c287bb0 > 1); the words come with it while another seat
    // plays (FUN_8c0bcc60), which it always does while one waits.
    static constexpr LegendsDreamcastPoint kWaitScroll{kArtLeft, 324};
    static constexpr s32 kWaitScrollWidth = 112;
    static constexpr s32 kWaitScrollRows = 16;
    static constexpr LegendsDreamcastPoint kWaitHereLine{8, 328};
    static constexpr Color kWaitHereInk = Color::rgba(0x16, 0x0C, 0x03);
    static constexpr s64 kWaitBlink = 4;
    static constexpr s32 kWaitLeastJoined = 2;

    /** FUN_8c0c2300 (8c0c2422..8c0c245a): the IT badge over the tagged player's box, its 64x64
     * picture squeezed into 32x32. */
    static constexpr LegendsDreamcastPoint kItBadge{26, 349};
    static constexpr s32 kItBadgeSize = 32;

    // FUN_8c0b7560's six kinds of card (names at 8c125004..8c12502c): gold, keys, food, food gone
    // bad, potions and powerups, each 128x128 under the taker's coloured strip, S3_W<colour>
    // (FUN_8c0b76a0). Dark Legacy's runestone, legend, crystal and gargoyle cards have none, and
    // keep Dark Legacy's own 128x64 card. Dark Legacy's motion carries them all.
    static constexpr std::array<LegendsDreamcastCardKind, 15> kCards{{
        {.card = "GOLD", .face = "GOLD"},
        {.card = "JUNK", .face = "GOLD"},
        {.card = "COINHUD", .face = "GOLD"},
        {.card = "KEY", .face = "KEY"},
        {.card = "KEY_RING", .face = "KEY"},
        {.card = "MEAT", .face = "FOOD"},
        {.card = "FRUIT", .face = "FOOD"},
        {.card = "BADMEAT", .face = "POISN_FOOD"},
        {.card = "BADFRUIT", .face = "POISN_FOOD"},
        {.card = "MAGIC", .face = "MAGIC"},
        {.card = "SPECIALS", .face = "SPECIALS"},
        {.card = "RUNESTONE", .face = {}},
        {.card = "LEGEND", .face = {}},
        {.card = "CRYSTAL", .face = {}},
        {.card = "GOLDNICON", .face = {}},
    }};
    /** A card hangs under its strip, which is this tall. */
    static constexpr s32 kCardStripRows = 16;

    /** The port's: the Dreamcast has no ticker. Dark Legacy's counts stand in Dark Legacy's rows,
     * centred on the box, written in font 1 at the size Dark Legacy writes them. */
    static constexpr f32 kTickerScale = StatusBoxPainter::kCountScale;

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

    // FUN_8c0ba060: the runestones held, along a player's strip, three of each colour 8 apart
    // and a unit more after each colour. FUN_8c0c08a0 shows them as it makes the box, and
    // FUN_8c0c2120 only ever refreshes them: they stay while the box does.
    static constexpr s32 kRuneLeft = -1;
    static constexpr s32 kRuneStep = 8;
    static constexpr s32 kRuneTop = 306;
    // FUN_8c0ba060 stands the bosses' keys from X+8 at 300, and FUN_8c0c2120 shows them only
    // while its relic timer runs. The Dreamcast's four stand 25 apart; Dark Legacy's eight keep
    // Dark Legacy's own spacing.
    static constexpr s32 kKeyLeft = 8;
    static constexpr s32 kKeyStep = StatusBoxPainter::kKeyStep;
    static constexpr s32 kKeyTop = 300;
    /** The thirteenth rune, which no Dreamcast box shows, where Dark Legacy's box shows it: 8
     * into the box's art, at 340. */
    static constexpr LegendsDreamcastPoint kThirteenthRune{kArtLeft + StatusBoxPainter::kRune13X,
                                                           StatusBoxPainter::kRune13Y};
    // 8c14bbf4: the legend items' five places between the gold and the health, 16 square,
    // crowded together; FUN_8c0c16e0 shows the Dreamcast's five items in them in turn.
    static constexpr std::array<LegendsDreamcastPoint, 5> kQuestPlaces{
        {{34, 362}, {42, 362}, {34, 370}, {42, 370}, {38, 367}}};
    static constexpr s32 kQuestIconSize = 16;
    /** Dark Legacy's legend items in realm order, the Dreamcast's five first, by their places
     * (8c1255f8: QUEST_ICON%d). */
    static constexpr std::array<LegendsDreamcastLegend, 9> kLegends{{
        {.realm = 1, .icon = "QUEST_ICON1", .picture = "SCIMITAR"},
        {.realm = 2, .icon = "QUEST_ICON2", .picture = "ICE_AX"},
        {.realm = 3, .icon = "QUEST_ICON3", .picture = "LAMP"},
        {.realm = 4, .icon = "QUEST_ICON4", .picture = "BILLOWS"},
        {.realm = 5, .icon = "QUEST_ICON5", .picture = "SOUL_SAVIOR"},
        {.realm = 7, .icon = {}, .picture = "BOOK"},
        {.realm = 9, .icon = {}, .picture = "FIRE_SCROLL"},
        {.realm = 10, .icon = {}, .picture = "LANTERN"},
        {.realm = 11, .icon = {}, .picture = "JAVILIN"},
    }};

    // FUN_8c09b360 makes the runestone finder's two blits at the top right, at x 392: THERMBASE
    // from 1 down (its blit's y of -1), with THERMCOL over it, which FUN_8c09b4e0 crops and
    // raises by Dark Legacy's finder's rule (RuneMeter::column), but straight with the
    // closeness, where Dark Legacy's rises with its square.
    static constexpr s32 kThermometerLeft = 392;
    static constexpr s32 kThermometerTop = 1;

    /** The powerups bank's frames of the hourglass's falling grains, SAND_ANIM00..04, which
     * Dark Legacy's falling counter steps through, held to the five: how fast the Dreamcast
     * steps them was not traced. FUN_8c09b640 stands the glass, the grains and the two crops of
     * sand where Dark Legacy's hourglass stands them (ChallengeHud's areas and sandAt). */
    static constexpr usize kSandFrames = 5;

    // FUN_8c071560 hangs a boss's meter from the frame's top, its strips' blits at y 383, each
    // strip 256 across from the meter's left. How the Dreamcast fills it and how opaque it
    // stands were not traced: it is cropped as Dark Legacy's, opaque and never tinted.
    static constexpr s32 kMeterTop = 1;
    static constexpr u8 kMeterOpacity = 255;

    /** The port's: the Dreamcast has no selector. The powerup a selector names is written in
     * font 1 over the box, centred on it, its bottom where Dark Legacy's label's is, at its own
     * size or the largest that keeps it within the box's width. */
    static constexpr s32 kLabelWidest = 96;

    // FUN_8c0e3420: a help message's scroll is (its longest line's characters + 6) x 9 across
    // and (its lines + 3) x 8 down. Its middle stands 54 over its anchor, which is 8 over the
    // point FUN_8c0808a0 projects, or, for a seat's message with no point, the seat's box's
    // centre 80 up from the frame's bottom. It is kept across the frame, its top from 2 down
    // and its bottom from 80 up, over the boxes. Its lines are written in font 1, 8 apart,
    // centred on it and down its middle, on the scroll of the costume colour of the seat it is
    // for at transparency 0x40, in that colour's ink.
    static constexpr s32 kHelpCharacterWidth = 9;
    static constexpr s32 kHelpMarginCharacters = 6;
    static constexpr s32 kHelpLineHeight = 8;
    static constexpr s32 kHelpMarginLines = 3;
    static constexpr s32 kHelpPointLift = 8;
    static constexpr s32 kHelpOverAnchor = 54;
    static constexpr s32 kHelpHighest = 2; ///< the scroll's top stands no higher
    /** 80 up from the frame's bottom: a seat's own anchor, and the lowest a scroll's bottom
     * stands. */
    static constexpr s32 kHelpFloor = 304;
    static constexpr u8 kHelpTransparency = 0x40;
    /** 8c14e7cc: the scrolls by costume colour, yellow, blue, red and green, and last the plain
     * one, for a message for nobody. */
    static constexpr std::array<std::string_view, 5> kHelpScrolls{
        "YEL_TEXTSCROLL", "BLU_TEXTSCROLL", "RED_TEXTSCROLL", "GRE_TEXTSCROLL", "TEXTSCROLL"};
    /** 8c14e7e0: the ink written on each. */
    static constexpr std::array<Color, 5> kHelpInks{
        Color::rgba(0x1F, 0x1F, 0x00), Color::rgba(0x00, 0x00, 0x1F), Color::rgba(0x1F, 0x00, 0x00),
        Color::rgba(0x00, 0x1F, 0x00), Color::rgba(0x16, 0x0C, 0x03)};
};

/** Seat `seat`'s column, and its box's centre. */
s32 dreamcastSeatX(usize seat);
s32 dreamcastSeatCentre(usize seat);

/** Which legend item, by realm, each of the box's five places shows for a player holding
 * `legends`, a bit per realm; none for a place left empty. The Dreamcast's five stand in their
 * own places (FUN_8c0c16e0); Dark Legacy's others take, in realm order, the places whose own item
 * is not held, and one left without a place is not shown. */
std::array<std::optional<s32>, LegendsDreamcastHudLayout::kQuestPlaces.size()>
dreamcastQuestPlaces(u16 legends);

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

/** A pickup card as the Dreamcast's boxes show it. */
struct LegendsDreamcastCard {
    std::string_view art; ///< its picture
    bool darkLegacy =
        false; ///< Dark Legacy's own STATIC card, for a kind the Dreamcast has none of
    bool operator==(const LegendsDreamcastCard&) const = default;
};

/** How the boxes show Dark Legacy's card `card`: the Dreamcast's card of its kind, or Dark
 * Legacy's own where the Dreamcast has none; none for a card Dark Legacy never shows. */
std::optional<LegendsDreamcastCard> dreamcastCard(std::string_view card);

/** Whether a seat's box says WAIT HERE... at the look's step `step` (FUN_8c0c16e0): while it
 * waits at an exit with `joined` seats in the game, more than one, four steps in eight. The
 * original has two more gates, untraced and taken as open: the box's timer at +0x1c2 at
 * nought (FUN_8c0bda40 counts it down in states 0 and 0xE) and *8c0bccd0 at nought
 * (FUN_8c0bcc60). */
bool dreamcastWaitHereShown(bool waiting, s32 joined, s64 step);

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

/** The runestone finder's column for Dark Legacy's fill `fill`, `width` across: Dark Legacy's
 * column for the closeness of which that fill is the square, as FUN_8c09b4e0 raises it. */
RuneMeter::Column dreamcastThermometerColumn(f32 fill, f32 width);

/** The size font 1, `font`, writes a selector's label `label` at: its own, else the largest at
 * which it keeps within the box's width. */
f32 dreamcastLabelScale(const TextPainter& font, std::string_view label);

/** Where a help message's scroll stands, and its lines on it, in whole canvas units. */
struct LegendsDreamcastHelpBox {
    s32 x = 0;
    s32 y = 0;
    s32 width = 0;
    s32 height = 0;
    s32 textCentre = 0; ///< the lines are centred on it
    s32 textTop = 0;    ///< the first line's top; each next stands kHelpLineHeight under it
    bool operator==(const LegendsDreamcastHelpBox&) const = default;
};

/** The scroll FUN_8c0e3420 puts under a message of `lines` for seat `seat`, -1 for one at a
 * place: over `head` when it is a projected point (`anchored`); over the seat's box when it is
 * not and the message has a seat; else over `head`, Dark Legacy's middle of the screen. */
LegendsDreamcastHelpBox dreamcastHelpBox(std::span<const std::string> lines, Vec2 head,
                                         bool anchored, s32 seat);

/** Which of the help scrolls, and of their inks, a message for costume colour `color` takes:
 * its own, else the plain one for a message for nobody. */
usize dreamcastHelpScroll(s32 color);

} // namespace gdl::game
