#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/assets/BitmapFont.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "expansions/n64/hud/LegendsN64HudIcons.h"
#include "expansions/n64/hud/LegendsN64HudLayout.h"
#include "expansions/n64/hud/LegendsN64Plaque.h"
#include "expansions/shared/hud/LegendsHudArt.h"
#include "expansions/shared/hud/LegendsHudClock.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/hud/BossMeter.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/StatusBox.h"
#include "game/players/PlayerControls.h"

namespace gdl::game {

/** The levels, 50 to 255, the turbo meter's green, yellow and red thirds are lit at while it
 * shows `shown` of 100 (FUN_e0059b44): each third brightens as it fills and is lit whole once
 * it is full. */
std::array<s32, 3> legendsN64TurboLevels(f32 shown);

/** One part of a span drawn from a mirrored tile: `length` frame units from `start`, across
 * which the texture's coordinate runs from `from` to `to`, falling where its texels run
 * backwards. */
struct LegendsN64SpanPart {
    f32 start = 0;
    f32 length = 0;
    f32 from = 0;
    f32 to = 0;
};

/** The parts of the span of `length` frame units from `start` whose first unit shows column `s`
 * of a tile mirrored about the first texel of a texture `size` texels across, as FUN_e002b184
 * sets such a tile up (its origin at the texture's width): the tile's columns below `size` run
 * backwards from the texture's last texel, those from `size` on forwards from its first. The
 * span lies within the tile's first twice `size` columns. */
std::array<std::optional<LegendsN64SpanPart>, 2> legendsN64MirroredSpan(f32 start, f32 length,
                                                                        f32 s, f32 size);

/** Where an advice scroll stands in the frame, and where its first line starts. */
struct LegendsN64HelpBox {
    s32 x = 0;
    s32 y = 0;
    s32 width = 0;
    s32 height = 0;
    s32 textX = 0; ///< the first line's left end
    s32 textY = 0; ///< and its top
    bool operator==(const LegendsN64HelpBox&) const = default;
};

/** The scroll FUN_e00717a0 sizes for `lines` lines of text, the widest `widest` frame units
 * across, at `lineHeight` a line: centred on frame point `centre`, in whole frame units, and
 * kept within the frame's sides, below its first row and above the panels. */
LegendsN64HelpBox legendsN64HelpBox(s32 widest, usize lines, s32 lineHeight, Vec2 centre);

/** How wide a two-strip meter's fills are drawn, each strip `strip` across, at `share` of it,
 * twice the health shown over the most (FUN_e0888a7c): the first from its cap of `leftInset`
 * to its end over the first half, the second from its start to `rightInset` short of its end
 * over the rest. */
std::array<s32, 2> legendsN64MeterWidths(f32 share, f32 strip, f32 leftInset, f32 rightInset);

/** The health a step moves every meter's shown health toward its boss's: a hundredth of
 * `maxHealth`, the maximum of the meter bound last, in whole health (FUN_e08887b8). */
s32 legendsN64MeterStep(f32 maxHealth);

/** The line the stats page writes for the stat named `label` at `value` (e00aad34, e00aad40):
 * the name right-aligned in eight characters and cut to them, a space, then the value in six
 * digits; the name and a space alone at nought. */
std::string legendsN64StatLine(std::string_view label, s32 value);

/** What the carousel writes under powerup `slot`: what Dark Legacy's selector counts it has
 * left, whole, as the N64 writes its seconds and charges; none at nought, or with nothing to
 * count. A powerup taken off keeps its count, which the N64 writes with its dimmed icon. */
std::optional<s32> legendsN64CarouselCount(const PowerupSlot& slot);

/** One of the quest page's entries. */
struct LegendsN64QuestEntry {
    enum class Kind : u8 {
        Legend,         ///< the legend item for realm `realm`'s boss
        ThirteenthRune, ///< the tower's own runestone
        Runestones,     ///< the twelve the realms hold, as a row
        Keys,           ///< the bosses' keys, as a row
    };
    Kind kind = Kind::Legend;
    s32 realm = 0;
    bool operator==(const LegendsN64QuestEntry&) const = default;
};

/** The quest page's entries, in the order it lists them. */
class LegendsN64QuestList {
public:
    /** Every legend item, the thirteenth rune, the runestones and the keys. */
    static constexpr usize kMost = kLegendsN64LegendRealms.size() + 3;

    std::span<const LegendsN64QuestEntry> entries() const {
        return std::span{m_entries}.first(m_count);
    }
    void add(LegendsN64QuestEntry entry);

private:
    std::array<LegendsN64QuestEntry, kMost> m_entries{};
    usize m_count = 0;
};

/** The quest page's entries for a seat holding the legend items `legends` (a bit a realm), the
 * runestones `runes` (StatusBoxView's, the thirteenth apart) and the bosses' keys `bossKeys`:
 * its legend items in their realms' order, then the thirteenth rune, then the runestones and
 * then the keys, each while it holds any. */
LegendsN64QuestList legendsN64QuestEntries(u16 legends, u16 runes, u16 bossKeys);

/**
 * The N64's look of the play HUD, drawn from the player's own N64 extraction in its 416x220
 * frame: a status panel along the bottom for each seat, lit in the costume's colour, with the
 * class's portrait, the turbo meter, health, keys, potions, the name and the class, or the
 * empty panel for a seat nobody plays and a player fallen to the tower; over a seat's panel,
 * the plaque naming the exit it stands on; Dark Legacy's pickup counts over the panels, in its
 * own font; the hourglass, the help on the player's coloured scroll and the boss meters across
 * the top. It shows Dark Legacy's state. A panel shows the powerups carousel while its selector
 * is up, and a seat played here browses its own stats and quest pages while the selector is
 * down; Dark Legacy's runestones, keys and legend items without N64 art show in Dark Legacy's
 * pictures there. An N64 exit's plaque is the N64's own picture of its level; one of Dark
 * Legacy's, which the N64 has none of, is the port's, naming the realm and the level in the
 * N64's fonts. The runestone finder is Dark Legacy's, as is the hourglass when the N64's timer
 * art is not there.
 */
class LegendsN64HudPainter final : public HudPainter {
public:
    static constexpr std::string_view kHudBank = "hud/objects.wad";
    static constexpr std::string_view kSmallFontFile = "Font/4x6font.fnt";  ///< counts and words
    static constexpr std::string_view kNameFontFile = "Font/glfm3.fnt";     ///< the name and help
    static constexpr std::string_view kNumberFontFile = "Font/numfont.FNT"; ///< health
    /** The hourglass's art, which the look can be drawn without. */
    static constexpr std::string_view kTimerBank = "screens/timer/objects.wad";
    /** The N64's pictures of its levels' names, a bank a realm (e00aae38..e00aae80), which the
     * look can be drawn without: an exit whose picture is not there shows no plaque. */
    static constexpr std::array<std::string_view, 5> kPlaqueBanks{
        "levelname/ice/objects.wad", "levelname/battle/objects.wad", "levelname/town/objects.wad",
        "levelname/castle/objects.wad", "levelname/mount/objects.wad"};
    /** Font 8, in which the port's plaques write their levels' names; without it, font 5. */
    static constexpr std::string_view kTitleFontFile = "Font/bigfont.fnt";
    /** The files of the N64 extraction the look cannot be drawn without, and so what the HUD
     * page's probe looks for before it offers the look: the HUD bank and the three fonts. */
    static constexpr std::array<std::string_view, 4> kRequiredFiles{kHudBank, kSmallFontFile,
                                                                    kNameFontFile, kNumberFontFile};

    LegendsN64HudPainter() = default;
    ~LegendsN64HudPainter() override = default;
    GDL_NON_COPYABLE_NON_MOVABLE(LegendsN64HudPainter);

    /** Takes the look's pictures and fonts from the N64 extraction `root`, the portraits of
     * `party`'s classes, and from `darkLegacy` the pickup counts' icons and the pictures the
     * carousel and the quest page show where the N64 has none; false when the HUD bank or a
     * font is missing. One that is there but cannot be read, or a HUD bank without a picture
     * the look draws, throws, naming it. A portrait bank that is missing or cannot be read
     * leaves its class's portrait out, a timer bank the N64's hourglass, a level-name bank its
     * exits' plaques, and the title font the port's plaques' level names to font 5. */
    bool load(RenderDevice& device, const std::filesystem::path& root, DarkLegacyHudArt& darkLegacy,
              std::span<const HudMember> party);

    std::string_view id() const override { return kLegendsN64HudStyle; }
    /** The canvas row of frame row 170, the panels' top: 300.28. */
    f32 statusTop() const override;
    /** Steps the look's clock with the play clock, eases the boss meters by it, fades each
     * panel's plaque in and out by it as its seat steps on and off an exit, and turns each
     * panel's page as its seat's selector and legend items change. */
    void prepare(HudFrame& frame, const HudView& view) override;
    void drawStatus(HudFrame& frame, const HudView& view) override;
    void drawRuneMeter(HudFrame& frame, const HudView& view) override;
    void drawHourglass(HudFrame& frame, const HudView& view) override;
    void drawBossMeters(HudFrame& frame, const HudView& view) override;
    /** Nothing: an open selector shows as its panel's carousel, and the N64 writes no
     * powerup's name over its panels. */
    void drawSelectors(HudFrame& frame, const HudView& view) override;
    void drawHelp(HudFrame& frame, const HudView& view) override;
    /** Down turns the panel from its normal page to the stats page, then the quest page while
     * it lists anything, then back; left and right go round the stats from either end and back
     * to the normal page past them, or round the quest page's entries (FUN_e00496d4). */
    void browse(s32 seat, const SelectorInput& presses) override;

private:
    /** What a panel shows. A seat's own page is never Powerups: the carousel shows while its
     * selector is up. */
    enum class Page : u8 { Normal, Stats, Quest, Powerups };
    /** Which of a panel's arrows a step lit. */
    enum class Arrow : u8 { Left, Right };
    /** A seat's own view of its panel, which only the look keeps. */
    struct SeatPages {
        Page page = Page::Normal;
        usize stat = 0;             ///< the stat the stats page shows, by its place in the ring
        LegendsN64QuestEntry quest; ///< the entry the quest page centres
        LegendsN64QuestList listed; ///< the quest page's entries when last prepared
        s32 selection = -1;         ///< the carousel's selection when last seen; -1 while hidden
        std::optional<Arrow> lit;   ///< the arrow the last step lit, until it settles
        bool swapped = false;       ///< the last step changed the icons, hidden until it settles
        bool stepped = false;       ///< browsed since the last prepare
        s64 steppedAt = 0;          ///< the look's step the last step was first shown at
        u16 legends = 0;            ///< the legend items held when last seen
        /** A legend item newly held while the panel showed the carousel, which the panel turns
         * to once the selector is down. */
        std::optional<LegendsN64QuestEntry> turnTo;
        bool seen = false;       ///< whether the look has seen the seat played
        LegendsN64Plaque plaque; ///< the exit the seat stands on, named over its panel
    };
    /** A picture the carousel or the quest page shows: its own size, or fitted into an icon's
     * square. */
    struct ItemPicture {
        const Texture* texture = nullptr;
        bool fitted = false;
    };
    /** A font the panels write in: its glyphs, its sheet and what writes with them. */
    struct Font {
        BitmapFont glyphs;
        std::unique_ptr<Texture> sheet;
        TextPainter text;
    };
    /** A class's portrait: lit in the costume's colour, with its shine over it. */
    struct Portrait {
        const Texture* light = nullptr;
        const Texture* shine = nullptr;
    };
    /** A boss meter's health as the look shows it, easing toward its boss's. */
    struct ShownMeter {
        f32 health = 0;
        f32 maxHealth = 0; ///< its most when last seen; another means another meter
        bool seen = false;
    };

    void release();
    void loadPictures(RenderDevice& device, const std::filesystem::path& bank);
    static void loadFont(RenderDevice& device, const std::filesystem::path& file,
                         std::string_view name, Font& font);
    void loadPortrait(RenderDevice& device, const AssetLocator& files, s32 classIndex);
    void loadTimer(RenderDevice& device, const AssetLocator& files);
    void loadPlaques(RenderDevice& device, const AssetLocator& files);
    void loadTitleFont(RenderDevice& device, const AssetLocator& files);
    void borrowPictures(DarkLegacyHudArt& darkLegacy);
    void easeMeters(const HudView& view, s64 steps, bool rewound);
    void turnPages(SeatPages& pages, const HudSeat& shown, bool rewound);
    /** Steps `pages`' plaque `steps` times for the exit `shown` stands on, or snaps it there
     * after a rewind. */
    void namePad(SeatPages& pages, const HudSeat& shown, s64 steps, bool rewound) const;
    /** What seat `seat`'s panel shows this frame. */
    Page pageOf(usize seat, const HudSeat& shown) const;
    void drawPanel(Canvas& canvas, usize seat, const HudSeat& shown) const;
    void drawEmpty(Canvas& canvas, f32 left, Color shade) const;
    void drawPowerups(Canvas& canvas, usize seat, const HudSeat& shown) const;
    void drawQuest(Canvas& canvas, usize seat, const HudSeat& shown) const;
    void drawWords(Canvas& canvas, usize seat, const HudSeat& shown, const HudView& view) const;
    void drawCarouselCount(Canvas& canvas, usize seat, const HudSeat& shown) const;
    void drawLights(Canvas& canvas, usize seat, const HudSeat& shown) const;
    void drawPlaque(Canvas& canvas, usize seat, const HudSeat& shown, const HudView& view) const;
    void drawCounts(Canvas& canvas, const HudView& view) const;
    ItemPicture powerupPicture(const PowerupSlot& slot) const;
    ItemPicture questPicture(const LegendsN64QuestEntry& entry, const HudSeat& shown) const;
    /** The picture of Dark Legacy's named `name` the look took as it opened; null for none. */
    const Texture* borrowed(std::string_view name) const;
    /** The scroll for a message in costume colour `color`; the plain one for none. */
    const Texture* helpScroll(s32 color) const;

    LegendsHudArt m_art;
    const Texture* m_color = nullptr;  ///< COLOR, lit in the costume's bright shade
    const Texture* m_empty = nullptr;  ///< COLORI, in a dark shade
    const Texture* m_border = nullptr; ///< BORDER
    const Texture* m_turbo = nullptr;  ///< TURBO, the meter's frame
    const Texture* m_lights = nullptr; ///< TURBOCLR, the meter's lights
    const Texture* m_heart = nullptr;  ///< HEART1
    const Texture* m_key = nullptr;    ///< KEY1
    const Texture* m_arrow = nullptr;  ///< ARROW
    std::array<Portrait, kClassCount> m_portraits{};
    const Texture* m_glass = nullptr; ///< TIMER_P0; none: the hourglass is Dark Legacy's
    const Texture* m_sand = nullptr;  ///< TIMER_P1
    std::array<const Texture*, LegendsN64HudLayout::kSandFrames> m_grains{}; ///< SAND_ANIM00..04
    /** Dark Legacy's pickup count icons, borrowed from its art as the look opened. */
    std::map<std::string, const Texture*, std::less<>> m_countIcons;
    /** Dark Legacy's INVENTORY and SELECT pictures of the items the N64 has no icon for,
     * borrowed as the look opened. */
    std::map<std::string, const Texture*, std::less<>> m_borrowed;
    /** Dark Legacy's STATIC runestones (SM_RUNE_), the thirteenth's and the bosses' keys
     * (SM_KEY_, by key), borrowed as the look opened. */
    std::array<const Texture*, static_cast<usize>(StatusBoxPainter::kRuneCount)> m_runes{};
    const Texture* m_thirteenthRune = nullptr;
    std::array<const Texture*, static_cast<usize>(StatusBoxPainter::kKeyCount)> m_keys{};
    Font m_small;   ///< font 2: the counts, the class, the stats, the prompt and the realms
    Font m_name;    ///< font 5: the player's name and the help
    Font m_numbers; ///< font 6: health
    Font m_title;   ///< font 8: the port's plaques' level names; none without its file
    LegendsHudClock m_clock;
    std::array<ShownMeter, BossMeters::kCapacity> m_meters{};
    std::array<SeatPages, HudView::kSeats> m_seats{};
};

} // namespace gdl::game
