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
 * 512x384 frame, the canvas's own: a status box along the bottom in each joined seat's column,
 * the stone strip over the costume's panel with the class's banner on it, the level, gold and
 * health with their labels, the keys and the potion thrown next while any are carried, the
 * turbo meter's arch, which glows once full, the runestones held along the strip, with the
 * bosses' keys over it a while, the thirteenth rune, the legend items held in the box's five
 * places, the IT badge over the tagged player's box, and WAIT HERE... blinking over a box whose
 * player waits at an exit for the others. A player fallen to the tower, or gone from the game,
 * shows the coloured kind-0 box, saying IN TOWER while it waits there or asking WAIT IN TOWER?
 * with YES marked, and NO while it may quit; a seat nobody joined shows nothing. The pickup
 * cards rise over the boxes on the taker's coloured strip, and Dark Legacy's counts stand over
 * them in the Dreamcast's small font. It shows Dark Legacy's state, and never the typed name. A
 * class with no Dreamcast banner is named in the Dreamcast's large font in the banner's place;
 * one with no costume bank takes the warrior's panel, as every class's panel of a colour is the
 * same picture; a legend item with no Dreamcast icon shows Dark Legacy's picture of it, a pickup
 * with no Dreamcast card Dark Legacy's card, and a count Dark Legacy's icon. The runestone
 * finder, the hourglasses, the boss meters, the selector labels and the help are Dark Legacy's.
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
    /** The powerups bank, which holds the legend items' icons. */
    static constexpr std::string_view kPowerupsFolder = "powerups";

    LegendsDreamcastHudPainter() = default;
    ~LegendsDreamcastHudPainter() override = default;
    GDL_NON_COPYABLE_NON_MOVABLE(LegendsDreamcastHudPainter);

    /** Takes the boxes' pictures and the fonts from the Dreamcast extraction's native folder
     * `root`, the panels and banners of `party`'s classes in their costumes' colours and the
     * legend items' icons; false when the static set or a font is missing. One that is there but
     * cannot be read, or a static set without a picture the boxes draw, throws, naming it. A
     * costume bank that is missing or cannot be read gives way to the warrior's panel, and its
     * banner to the class's name; a powerups bank that is missing or cannot be read, or an icon
     * it lacks, to Dark Legacy's picture of the item. Of Dark Legacy's art it borrows SELECT's
     * pictures of the legend items it has no icon for, STATIC's cards of the pickups it has no
     * card for and every icon a pickup count shows, and nothing else. */
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
    void loadLegends(RenderDevice& device, const AssetLocator& files, DarkLegacyHudArt& darkLegacy);
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
    void drawPanel(Canvas& canvas, usize seat, const HudSeat& shown, const HudView& view) const;
    void drawPlain(Canvas& canvas, usize seat, s32 color) const;
    void drawNumbers(Canvas& canvas, usize seat, const HudSeat& shown) const;
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
    const Texture* m_strip = nullptr;          ///< BK_RUNE_STONE_02
    const Texture* m_levelLabel = nullptr;     ///< BTMBK_LEVL
    const Texture* m_goldLabel = nullptr;      ///< BTMBK_GOLD
    const Texture* m_healthLabel = nullptr;    ///< BTMBK_HELTH
    const Texture* m_key = nullptr;            ///< KEY_ICON
    const Texture* m_glow = nullptr;           ///< TURBO_GLOW
    const Texture* m_thirteenthRune = nullptr; ///< RUNE13
    const Texture* m_it = nullptr;             ///< IT
    const Texture* m_waitScroll = nullptr;     ///< TEXTSCROLL
    ColourPictures m_plainStrips{};            ///< S3_W<colour>
    ColourPictures m_plainPanels{};            ///< S4_W<colour>
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
