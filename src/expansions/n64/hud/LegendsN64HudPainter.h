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

#include "expansions/n64/hud/LegendsN64HudLayout.h"
#include "expansions/shared/hud/LegendsHudArt.h"
#include "expansions/shared/hud/LegendsHudClock.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "game/characters/ClassData.h"
#include "game/hud/BossMeter.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"

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

/**
 * The N64's look of the play HUD, drawn from the player's own N64 extraction in its 416x220
 * frame: a status panel along the bottom for each seat, lit in the costume's colour, with the
 * class's portrait, the turbo meter, health, keys, potions, the name and the class, or the
 * empty panel for a seat nobody plays and a player fallen to the tower; Dark Legacy's pickup
 * counts over the panels, in its own font; the hourglass, the help on the player's coloured
 * scroll and the boss meters across the top. It shows Dark Legacy's state. The runestone finder
 * and the selector labels are Dark Legacy's, as is the hourglass when the N64's timer art is not
 * there.
 */
class LegendsN64HudPainter final : public HudPainter {
public:
    static constexpr std::string_view kHudBank = "hud/objects.wad";
    static constexpr std::string_view kSmallFontFile = "Font/4x6font.fnt";  ///< counts and words
    static constexpr std::string_view kNameFontFile = "Font/glfm3.fnt";     ///< the name and help
    static constexpr std::string_view kNumberFontFile = "Font/numfont.FNT"; ///< health
    /** The hourglass's art, which the look can be drawn without. */
    static constexpr std::string_view kTimerBank = "screens/timer/objects.wad";
    /** The files of the N64 extraction the look cannot be drawn without, and so what the HUD
     * page's probe looks for before it offers the look: the HUD bank and the three fonts. */
    static constexpr std::array<std::string_view, 4> kRequiredFiles{kHudBank, kSmallFontFile,
                                                                    kNameFontFile, kNumberFontFile};

    LegendsN64HudPainter() = default;
    ~LegendsN64HudPainter() override = default;
    GDL_NON_COPYABLE_NON_MOVABLE(LegendsN64HudPainter);

    /** Takes the look's pictures and fonts from the N64 extraction `root`, the portraits of
     * `party`'s classes, and the pickup counts' icons from `darkLegacy`; false when the HUD bank
     * or a font is missing. One that is there but cannot be read, or a HUD bank without a
     * picture the look draws, throws, naming it. A portrait bank that is missing or cannot be
     * read leaves its class's portrait out, and a timer bank the N64's hourglass. */
    bool load(RenderDevice& device, const std::filesystem::path& root, DarkLegacyHudArt& darkLegacy,
              std::span<const HudMember> party);

    std::string_view id() const override { return kLegendsN64HudStyle; }
    /** The canvas row of frame row 170, the panels' top: 300.28. */
    f32 statusTop() const override;
    /** Steps the look's clock with the play clock and eases the boss meters by it. */
    void prepare(HudFrame& frame, const HudView& view) override;
    void drawStatus(HudFrame& frame, const HudView& view) override;
    void drawRuneMeter(HudFrame& frame, const HudView& view) override;
    void drawHourglass(HudFrame& frame, const HudView& view) override;
    void drawBossMeters(HudFrame& frame, const HudView& view) override;
    void drawSelectors(HudFrame& frame, const HudView& view) override;
    void drawHelp(HudFrame& frame, const HudView& view) override;

private:
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
    void borrowCountIcons(DarkLegacyHudArt& darkLegacy);
    void easeMeters(const HudView& view, s64 steps, bool rewound);
    void drawPanel(Canvas& canvas, usize seat, const HudSeat& shown) const;
    void drawEmpty(Canvas& canvas, f32 left, Color shade) const;
    void drawWords(Canvas& canvas, usize seat, const HudSeat& shown, const HudView& view) const;
    void drawLights(Canvas& canvas, usize seat, const HudSeat& shown) const;
    void drawCounts(Canvas& canvas, const HudView& view) const;
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
    Font m_small;   ///< font 2: the counts, the class and the prompt
    Font m_name;    ///< font 5: the player's name and the help
    Font m_numbers; ///< font 6: health
    LegendsHudClock m_clock;
    std::array<ShownMeter, BossMeters::kCapacity> m_meters{};
};

} // namespace gdl::game
