#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "engine/assets/BitmapFont.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "expansions/shared/hud/LegendsHudArt.h"
#include "expansions/shared/hud/LegendsHudClock.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "game/characters/ClassData.h"
#include "game/characters/Relics.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/StatusBox.h"

namespace gdl::game {

/**
 * The Dreamcast's look of the play HUD, drawn from the player's own Dreamcast extraction on its
 * 512x384 frame, the canvas's own: each seat's status box, a seat nobody joined showing the
 * coloured kind-0 box, with the middle pieces between the second and third, the pickup cards
 * and counts over the boxes, the runestone finder, the hourglass, the boss meters, the
 * selectors' labels and the help. It shows Dark Legacy's state, never the typed name, and where
 * the extraction has no art for a part it shows Dark Legacy's. LegendsDreamcastHudLayout says
 * where each part stands.
 */
class LegendsDreamcastHudPainter final : public HudPainter {
public:
    /** The static set, which holds the boxes' art and the fonts' sheets. */
    static constexpr std::string_view kStaticObjects = "static/objects.rom";
    static constexpr std::string_view kStaticTextures = "static/textures.rom";
    static constexpr std::string_view kSmallFontFile = "fonts/8hi_fonts5.fnt"; ///< the level, words
    static constexpr std::string_view kScoreFontFile = "fonts/score.fnt";  ///< gold, health, counts
    static constexpr std::string_view kLargeFontFile = "fonts/font32.fnt"; ///< a class's name
    /** The files of the Dreamcast extraction the look cannot be drawn without, and so what the
     * HUD page's probe looks for before it offers the look: the static set and the three
     * fonts. */
    static constexpr std::array<std::string_view, 5> kRequiredFiles{
        kStaticObjects, kStaticTextures, kSmallFontFile, kScoreFontFile, kLargeFontFile};
    /** The powerups bank, which holds the legend items' icons and the hourglass. */
    static constexpr std::string_view kPowerupsFolder = "powerups";

    LegendsDreamcastHudPainter() = default;
    ~LegendsDreamcastHudPainter() override = default;
    GDL_NON_COPYABLE_NON_MOVABLE(LegendsDreamcastHudPainter);

    /** Takes all the look's art at once from the Dreamcast extraction's native folder `root`,
     * `party`'s classes' panels and banners among it, and the pictures it borrows from Dark
     * Legacy's art. False when a required file is missing; one that is there but cannot be
     * read, or a static set without a picture the look draws, throws, naming it. A costume or
     * powerups bank that is missing or unreadable gives way to its parts' stand-ins. */
    bool load(RenderDevice& device, const std::filesystem::path& root, DarkLegacyHudArt& darkLegacy,
              std::span<const HudMember> party);

    std::string_view id() const override { return kLegendsDreamcastHudStyle; }
    /** The kind-0 box's strip's top, 304. */
    f32 statusTop() const override;
    /** Steps the look's clock with the play clock, and each full arch's glow by it. */
    void prepare(HudFrame& frame, const HudView& view) override;
    void drawStatus(HudFrame& frame, const HudView& view) override;
    void drawRuneMeter(HudFrame& frame, const HudView& view) override;
    void drawHourglass(HudFrame& frame, const HudView& view) override;
    void drawBossMeters(HudFrame& frame, const HudView& view) override;
    void drawSelectors(HudFrame& frame, const HudView& view) override;
    void drawHelp(HudFrame& frame, const HudView& view) override;

private:
    /** A font the boxes write in: its glyphs, and what writes them from its sheet. */
    struct Font {
        BitmapFont glyphs;
        TextPainter text;
    };
    /** A class's art in one costume colour. */
    struct ClassArt {
        const Texture* panel = nullptr;  ///< BTMBK_<colour>
        const Texture* banner = nullptr; ///< <CLS>_NAME
        bool taken = false;              ///< whether the look has looked for it
    };
    /** A legend item's picture: the Dreamcast's icon, or Dark Legacy's, fitted into its place. */
    struct LegendArt {
        const Texture* picture = nullptr;
        bool fitted = false; ///< Dark Legacy's picture, of its own shape
    };
    using ColourPictures = std::array<const Texture*, kColorCount>;

    void release();
    void loadStatic(RenderDevice& device, const std::filesystem::path& objects,
                    const std::filesystem::path& textures);
    void loadFont(const std::filesystem::path& file, std::string_view name, s32 space,
                  std::string_view sheet, Font& font);
    void loadClass(RenderDevice& device, const AssetLocator& files, s32 classIndex, s32 color);
    /** Takes the legend items' icons from the powerups bank's `pictures`, none when it is not
     * there, and Dark Legacy's pictures of the rest. */
    void loadLegends(RenderDevice& device, std::span<const formats::LegendsMapTexture> pictures,
                     DarkLegacyHudArt& darkLegacy);
    /** Takes the hourglass from the powerups bank's `pictures`: all of it, or none. */
    void loadTimer(RenderDevice& device, std::span<const formats::LegendsMapTexture> pictures);
    /** Takes Dark Legacy's cards of the pickups the Dreamcast has none for, and the icons of its
     * counts, so that drawing looks nothing up. */
    void borrowPictures(DarkLegacyHudArt& darkLegacy);
    /** Picture `name` of the bank in `folder`, as `pictures` decoded it, uploaded once under the
     * bank's name and its own; null when the bank has none. */
    const Texture* takePicture(RenderDevice& device,
                               std::span<const formats::LegendsMapTexture> pictures,
                               const std::string& folder, std::string_view name);
    /** The art of class `classIndex` in costume colour `color`; null for none the look took. */
    const ClassArt* classArtOf(s32 classIndex, s32 color) const;
    /** Seat `seat`'s strip and panel, the deepest of its box: the stone strip and the costume's
     * panel of a player's box, or a kind-0 box's coloured pair. */
    void drawBack(Canvas& canvas, usize seat, const HudSeat& shown) const;
    /** The rest of seat `seat`'s box, over every seat's strip and panel. */
    void drawFront(Canvas& canvas, usize seat, const HudSeat& shown, const HudView& view) const;
    /** What a player's box shows over its strip and panel: the banner, the numbers, what is
     * carried, the arch, the relics and the legend items. */
    void drawPanel(Canvas& canvas, usize seat, const HudSeat& shown, const HudView& view) const;
    void drawPlain(Canvas& canvas, usize seat, s32 color) const;
    void drawNumbers(Canvas& canvas, usize seat, const StatusBoxView& box) const;
    void drawCarried(Canvas& canvas, usize seat, const StatusBoxView& box) const;
    void drawTurbo(Canvas& canvas, usize seat, const HudSeat& shown) const;
    void drawRelics(Canvas& canvas, usize seat, const StatusBoxView& box, bool keysShown) const;
    void drawLegends(Canvas& canvas, usize seat, u16 legends) const;
    void drawIt(Canvas& canvas, usize seat) const;
    void drawWaitHere(Canvas& canvas, usize seat, const HudSeat& shown, const HudView& view) const;
    /** What a fallen player's box says: IN TOWER while it waits there, or the prompt. */
    void drawFallen(Canvas& canvas, usize seat, const HudSeat& shown, const HudView& view) const;
    void drawCards(Canvas& canvas, const HudView& view) const;
    void drawCounts(Canvas& canvas, const HudView& view) const;

    LegendsHudArt m_art;
    const Texture* m_strip = nullptr;             ///< BK_RUNE_STONE_02
    const Texture* m_levelLabel = nullptr;        ///< BTMBK_LEVL
    const Texture* m_goldLabel = nullptr;         ///< BTMBK_GOLD
    const Texture* m_healthLabel = nullptr;       ///< BTMBK_HELTH
    const Texture* m_key = nullptr;               ///< KEY_ICON
    const Texture* m_glow = nullptr;              ///< TURBO_GLOW
    const Texture* m_thirteenthRune = nullptr;    ///< RUNE13
    const Texture* m_it = nullptr;                ///< IT
    const Texture* m_waitScroll = nullptr;        ///< TEXTSCROLL
    const Texture* m_middleStrip = nullptr;       ///< S3_MID
    const Texture* m_middlePanel = nullptr;       ///< S4_MID
    const Texture* m_thermometerBase = nullptr;   ///< THERMBASE
    const Texture* m_thermometerColumn = nullptr; ///< THERMCOL
    const Texture* m_timer = nullptr;             ///< TIMER; none: the hourglass is Dark Legacy's
    const Texture* m_timerSand = nullptr;         ///< TIMER_SAND
    /** SAND_ANIM00..04, the falling grains' frames. */
    std::array<const Texture*, LegendsDreamcastHudLayout::kSandFrames> m_grains{};
    /** The help scrolls by costume colour, the plain one last. */
    std::array<const Texture*, LegendsDreamcastHudLayout::kHelpScrolls.size()> m_helpScrolls{};
    ColourPictures m_plainStrips{}; ///< S3_W<colour>
    ColourPictures m_plainPanels{}; ///< S4_W<colour>
    /** SM_RUNE_<colour>_0<n>, the runestones in their order. */
    std::array<const Texture*, StatusBoxPainter::kRuneCount> m_runes{};
    /** SM_KEY_<colour>, the bosses' keys in their order, which share the four colours' pictures. */
    std::array<const Texture*, StatusBoxPainter::kKeyCount> m_keys{};
    std::array<LegendArt, Relics::kRealmCount> m_legends{}; ///< by the realm of each item's boss
    std::array<std::array<ClassArt, kColorCount>, kClassCount> m_classes{};
    /** Dark Legacy's cards of the pickups the Dreamcast has none for, by name; borrowed. */
    std::map<std::string, const Texture*, std::less<>> m_darkLegacyCards;
    /** Dark Legacy's pictures of what a pickup count counts, by name; borrowed. */
    std::map<std::string, const Texture*, std::less<>> m_countIcons;
    Font m_small; ///< font 1: the level and the box's words
    Font m_score; ///< font 4: gold, health and the counts
    Font m_large; ///< font 6: a class's name, where it has no banner
    LegendsHudClock m_clock;
    /** How many of the look's steps each seat's arch has stood full, which its glow's pulse
     * follows: the pulse waits while the arch is not full, as FUN_8c0c13e0 leaves it. */
    std::array<s64, HudView::kSeats> m_glowSteps{};
};

} // namespace gdl::game
