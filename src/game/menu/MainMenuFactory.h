#pragma once

#include <span>
#include <string>
#include <utility>

#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"

#include "game/widgets/OptionMenu.h"

namespace gdl::game {

/** One row of the title screen's menu. */
struct MainMenuEntry {
    std::string text;
    s32 code = 0;
    bool enabled = true;
};

/**
 * Builds the title screen's menus by one rule, so a row registered later neither overlaps
 * the logo above nor leaves the screen below: a bare column centred on the screen's width,
 * its first row where the retail "Press Start" row stands when the column fits, climbing
 * no higher than the logo's foot and shrinking its rows before it would.
 */
class MainMenuFactory {
public:
    static constexpr s32 kRowTop = 304;     ///< the first row's place when the column fits
    static constexpr s32 kHighestTop = 256; ///< the logo's foot: no row stands above it
    static constexpr s32 kBottomMargin = 8; ///< kept clear under the last row

    /** The first row's y and the rows' scale for `rows` rows `lineHeight` tall at scale one
     * on a screen `height` tall. */
    static std::pair<s32, f32> place(s32 rows, s32 lineHeight, s32 height);
    /** The menu for `entries`, laid out by `place` with the painter's font. */
    static MenuDefinition build(std::span<const MainMenuEntry> entries,
                                const TextPainter& painter, const MenuScreen& screen);
};

} // namespace gdl::game
