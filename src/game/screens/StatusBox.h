#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "game/players/TurboMeter.h"

namespace gdl::game {

/** What one player's status box shows. */
struct StatusBoxView {
    enum class Mode : u8 {
        Plain,     ///< the stone panel alone
        Character, ///< the class icon and name
        Status     ///< the class icon, name, level, gold and health
    };

    Mode mode = Mode::Plain;
    bool active = false; ///< a player owns the box (else it is dimmed)
    s32 classIndex = 0;
    s32 color = 0;
    std::string name;
    s32 level = 1;
    s32 gold = 0;
    s32 health = 0;
    s32 keys = 0; ///< shown with their icon when any are carried
    s32 potions = 0;
    s32 potionKind = 0;       ///< of the potion thrown next, which picks the icon's colour
    bool inTower = false;     ///< fallen: the box says so in place of what is carried
    bool towerPrompt = false; ///< fallen outside the tower: wait there, or quit the game
    std::optional<TurboMeterLook> turbo; ///< the turbo meter over the box, when it has one
    u16 runes = 0;                       ///< the runestones held, a bit each (retail's shards)
    u16 bossKeys = 0;                    ///< the bosses' keys, a bit per realm in the tower's order
    bool keysShown = false;              ///< the keys show only a while after a level opens
};

/**
 * The status boxes along the bottom of the screen, one per player: the stone panel tinted by
 * costume, the class icon panel once a character is chosen, and the name, level, gold and
 * health in the original's fonts.
 */
class StatusBoxPainter {
public:
    static constexpr s32 kTurboY = 304; ///< the turbo meter's sheets, the box's width
    static constexpr s32 kGleamX = 80;  ///< its gleam, within the box
    static constexpr s32 kGleamY = 310;
    static constexpr s32 kInTowerY = 340; ///< where a fallen character's box says so
    static constexpr f32 kInTowerScale = 1.2f;
    static constexpr s32 kPromptIconX = 6; ///< the prompt's buttons, within the box
    static constexpr s32 kPromptTextX = 20;
    static constexpr s32 kPromptIconSize = 14;
    static constexpr s32 kWaitIconY = 332; ///< BUTTON_X, to wait in the tower
    static constexpr s32 kWaitTextY = 336;
    static constexpr s32 kQuitIconY = 352; ///< BUTTON_TRI, to quit the game
    static constexpr s32 kQuitTextY = 356;
    static constexpr s32 kCarriedY = 323; ///< the key and potion icons' top
    static constexpr s32 kCarriedTextY = 327;
    static constexpr s32 kKeyIconX = 8;
    static constexpr s32 kKeyCountX = 26;
    static constexpr s32 kPotionIconX = 102;
    static constexpr s32 kPotionCountX = 92;
    static constexpr f32 kCarriedScale = 0.8f;
    /** The potion icon for each kind; an unknown kind shows as red. */
    static constexpr std::array<std::string_view, 5> kPotionIcons{
        "POTION_ICON_RED", "POTION_ICON_RED", "POTION_ICON_BLU", "POTION_ICON_YEL",
        "POTION_ICON_GRE"};
    static std::string_view potionIcon(s32 kind);
    // The relic strip (ShowRuneStones, player.c 5125): twelve runestones and eight keys.
    static constexpr s32 kRuneCount = 12;
    static constexpr s32 kRuneX = 15; ///< the first rune within the box, then 8 on, a pixel a trio
    static constexpr s32 kRuneStep = 8;
    static constexpr s32 kRunesInColour = 3;
    static constexpr s32 kRuneY = 306;
    static constexpr s32 kKeyCount = 8;
    static constexpr s32 kKeyX = 12;
    static constexpr s32 kKeyStep = 12;
    static constexpr s32 kKeyY = 300;
    /** The colours the runes run through, three each, and the keys, blue to green twice. */
    static constexpr std::array<std::string_view, 4> kRelicColours{"BLU", "RED", "YEL", "GRE"};
    static constexpr s32 kWidth = 128;
    static constexpr s32 kY = 320;
    static constexpr s32 kHeight = 64;
    static constexpr s32 kBarY = 304;
    static constexpr s32 kBarHeight = 16;

    /** Loads the panels and fonts from the unpacked data; false when they are missing. */
    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const StringTable* strings);
    void release();
    bool loaded() const { return m_device != nullptr; }
    /** The small capitals the boxes' levels are written in, which the help messages share. */
    const TextPainter& smallCaps() const { return m_smallCaps; }

    /** Draws player `slot`'s box; `bar` adds the strip above it that the game shows in play. */
    void draw(Canvas& canvas, s32 slot, const StatusBoxView& view, bool bar);
    /** Draws a pickup's strip at `y` over slot `slot`, the STATIC `card` hanging under it. */
    void drawCard(Canvas& canvas, s32 slot, std::string_view card, s32 y);
    /** The turbo meter over a box: the bar behind, the front colour grown from its middle,
     * the glint, the glow of a full one and the gleam of a change. */
    void drawTurbo(Canvas& canvas, s32 slot, const TurboMeterLook& look);
    /** The runestones held (`SM_RUNE_<colour>_0<n>`) and, when `keys`, the bosses' keys
     * (`SM_KEY_<colour>`) along the top of slot `slot`'s box. */
    void drawRelics(Canvas& canvas, s32 slot, u16 runes, std::optional<u16> keys);
    /** Draws a pickup count above slot `slot`: the STATIC `icon`, then "count/total". */
    void drawCount(Canvas& canvas, s32 slot, std::string_view icon, s32 count, s32 total);
    void setCountTextures(TextureSet* textures) { m_countTextures = textures; }

private:
    const Texture* selectTexture(std::string_view name);
    const Texture* staticTexture(std::string_view name);
    std::string_view text(std::string_view id) const;

    RenderDevice* m_device = nullptr;
    const StringTable* m_strings = nullptr;
    TextureSet m_select;
    TextureSet m_static;
    TextureSet* m_countTextures = nullptr; ///< borrowed level-specific pickup icons
    BitmapFont m_fontInitials;
    BitmapFont m_fontScore;
    BitmapFont m_fontSmallCaps;
    TextPainter m_initials;
    TextPainter m_score;
    TextPainter m_smallCaps;
};

} // namespace gdl::game
