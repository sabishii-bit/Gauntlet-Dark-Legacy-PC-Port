#pragma once

#include <string_view>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"

#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudView.h"
#include "game/players/PlayerControls.h"

namespace gdl::game {

/** What a painter draws with in one frame. */
struct HudFrame {
    Canvas& canvas;
    RenderDevice& device;
    DarkLegacyHudArt& darkLegacy;
};

/** One look of the play HUD. It reads the frame's view and never changes the game; its art
 * and its clocks are its own, and it takes all its art as it opens. Each draw is one of the
 * scene's HUD slots. */
class HudPainter {
public:
    virtual ~HudPainter() = default;
    /** The look's id, as the settings name it. */
    virtual std::string_view id() const = 0;
    /** The canvas row the panels start at; the companion arrows stay above it. */
    virtual f32 statusTop() const = 0;
    /** Once a frame before the draws: clocks and edges. Never loads, uploads or draws. */
    virtual void prepare(HudFrame& /*frame*/, const HudView& /*view*/) {}
    /** The boxes, with the cards, counts and plaques over them. */
    virtual void drawStatus(HudFrame& frame, const HudView& view) = 0;
    virtual void drawRuneMeter(HudFrame& frame, const HudView& view) = 0;
    virtual void drawHourglass(HudFrame& frame, const HudView& view) = 0;
    virtual void drawBossMeters(HudFrame& frame, const HudView& view) = 0;
    virtual void drawSelectors(HudFrame& frame, const HudView& view) = 0;
    virtual void drawHelp(HudFrame& frame, const HudView& view) = 0;
    /** A local seat's selector presses while its selector is closed: the look's own pages. */
    virtual void browse(s32 /*seat*/, const SelectorInput& /*presses*/) {}
    /** A local seat's left and right while its selector stands open, which move the selection:
     * a look may show which way each went, whether or not another powerup comes up. */
    virtual void steer(s32 /*seat*/, const SelectorInput& /*presses*/) {}
};

} // namespace gdl::game
