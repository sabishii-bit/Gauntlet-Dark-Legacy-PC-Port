#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/players/TurboMeter.h"

namespace gdl::game {

/** What one player's status box shows. */
struct StatusBoxView {
    enum class Mode : std::uint8_t {
        Plain,     ///< the stone panel alone
        Character, ///< the class icon and name
        Status     ///< the class icon, name, level, gold and health
    };

    Mode mode = Mode::Plain;
    bool active = false; ///< a player owns the box (else it is dimmed)
    int classIndex = 0;
    int color = 0;
    std::string name;
    int level = 1;
    int gold = 0;
    int health = 0;
    int keys = 0; ///< shown with their icon when any are carried
    int potions = 0;
    int potionKind = 0;   ///< of the potion thrown next, which picks the icon's colour
    bool inTower = false; ///< fallen: the box says so in place of what is carried
    std::optional<TurboMeterLook> turbo; ///< the turbo meter over the box, when it has one
};

/**
 * The status boxes along the bottom of the screen, one per player: the stone panel tinted by
 * costume, the class icon panel once a character is chosen, and the name, level, gold and
 * health in the original's fonts.
 */
class StatusBoxPainter {
public:
    static constexpr int kTurboY = 304; ///< the turbo meter's sheets, the box's width
    static constexpr int kGleamX = 80;  ///< its gleam, within the box
    static constexpr int kGleamY = 310;
    static constexpr int kInTowerY = 340; ///< where a fallen character's box says so
    static constexpr float kInTowerScale = 1.2f;
    static constexpr int kCarriedY = 323; ///< the key and potion icons' top
    static constexpr int kCarriedTextY = 327;
    static constexpr int kKeyIconX = 8;
    static constexpr int kKeyCountX = 26;
    static constexpr int kPotionIconX = 102;
    static constexpr int kPotionCountX = 92;
    static constexpr float kCarriedScale = 0.8f;
    /** The potion icon for each kind; an unknown kind shows as red. */
    static constexpr std::array<std::string_view, 5> kPotionIcons{
        "POTION_ICON_RED", "POTION_ICON_RED", "POTION_ICON_BLU", "POTION_ICON_YEL",
        "POTION_ICON_GRE"};
    static std::string_view potionIcon(int kind);
    static constexpr int kWidth = 128;
    static constexpr int kY = 320;
    static constexpr int kHeight = 64;
    static constexpr int kBarY = 304;
    static constexpr int kBarHeight = 16;

    /** Loads the panels and fonts from the unpacked data; false when they are missing. */
    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const StringTable* strings);
    void release();
    bool loaded() const { return m_device != nullptr; }
    /** The small capitals the boxes' levels are written in, which the help messages share. */
    const TextPainter& smallCaps() const { return m_smallCaps; }

    /** Draws player `slot`'s box; `bar` adds the strip above it that the game shows in play. */
    void draw(Canvas& canvas, int slot, const StatusBoxView& view, bool bar);
    /** Draws a pickup's strip at `y` over slot `slot`, the STATIC `card` hanging under it. */
    void drawCard(Canvas& canvas, int slot, std::string_view card, int y);
    /** The turbo meter over a box: the bar behind, the front colour grown from its middle,
     * the glint, the glow of a full one and the gleam of a change. */
    void drawTurbo(Canvas& canvas, int slot, const TurboMeterLook& look);
    /** Draws a pickup count above slot `slot`: the STATIC `icon`, then "count/total". */
    void drawCount(Canvas& canvas, int slot, std::string_view icon, int count, int total);

private:
    const Texture* selectTexture(std::string_view name);
    const Texture* staticTexture(std::string_view name);
    std::string_view text(std::string_view id) const;

    RenderDevice* m_device = nullptr;
    const StringTable* m_strings = nullptr;
    TextureSet m_select;
    TextureSet m_static;
    BitmapFont m_fontInitials;
    BitmapFont m_fontScore;
    BitmapFont m_fontSmallCaps;
    TextPainter m_initials;
    TextPainter m_score;
    TextPainter m_smallCaps;
};

} // namespace gdl::game
