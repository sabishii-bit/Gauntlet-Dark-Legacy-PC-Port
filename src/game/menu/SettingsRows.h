#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "engine/ui/TextPainter.h"

#include "game/widgets/OptionMenu.h"

namespace gdl::game {
/** The one way the settings pages make their rows, so every page reads and fits alike: a
 * choice that leads somewhere, a marked choice among alternatives, a row with a value beside
 * it, an On/Off switch; a column at the choice pages' size; a switch column; and the scale
 * that keeps the longest row on the parchment. */
class SettingsRows {
public:
    using Text = std::function<std::string(std::string_view)>;
    /** The choice pages' column: from (96, 128) at seven tenths, the cursor to match. */
    static constexpr s32 kColumnX = 96;
    static constexpr s32 kColumnY = 128;
    static constexpr f32 kChoiceScale = 0.7f;
    static constexpr s32 kSwitchX = 344; ///< where a page's switches stand
    static constexpr s32 kMargin = 64;   ///< kept clear at either side of the parchment

    SettingsRows(MenuDefinition& definition, const TextPainter& painter, Text text);
    MenuItem& choice(std::string label, s32 code);
    MenuItem& marked(std::string label, s32 code, bool marked);
    MenuItem& value(std::string label, s32 code, std::string value, bool enabled = true);
    MenuItem& toggle(std::string label, s32 code, bool on);
    /** Rows down a column from (x, y) at `scale`, the cursor the same size. */
    void column(s32 x = kColumnX, s32 y = kColumnY, f32 scale = kChoiceScale);
    /** The switch column at x, as wide as the wider of On and Off at the page's scale. */
    void switches(s32 x = kSwitchX);
    /** The scale, at most `most`, that keeps every label (with `tail` after it) between the
     * margins of a screen `width` wide; the cursor follows. */
    void fit(f32 most, s32 width, std::string_view tail = {});

private:
    MenuDefinition& m_definition;
    const TextPainter& m_painter;
    Text m_text;
};
} // namespace gdl::game
