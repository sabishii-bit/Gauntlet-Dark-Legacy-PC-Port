#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

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
};

/**
 * The status boxes along the bottom of the screen, one per player: the stone panel tinted by
 * costume, the class icon panel once a character is chosen, and the name, level, gold and
 * health in the original's fonts.
 */
class StatusBoxPainter {
public:
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

    /** Draws player `slot`'s box; `bar` adds the strip above it that the game shows in play. */
    void draw(Canvas& canvas, s32 slot, const StatusBoxView& view, bool bar);
    /** Draws a pickup's strip at `y` over slot `slot`, the STATIC `card` hanging under it. */
    void drawCard(Canvas& canvas, s32 slot, std::string_view card, s32 y);
    /** Draws a pickup count above slot `slot`: the STATIC `icon`, then "count/total". */
    void drawCount(Canvas& canvas, s32 slot, std::string_view icon, s32 count, s32 total);

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
