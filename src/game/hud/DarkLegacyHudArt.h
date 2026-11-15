#pragma once

#include <string_view>

#include "engine/render/RenderTypes.h"
#include "engine/ui/TextPainter.h"

namespace gdl::game {

class HudPainter;

/** What the other HUD looks borrow from Dark Legacy's: its boxes' STATIC and SELECT pictures,
 * the pickup count icons, INVENTORY's powerup pictures, the strings' small capitals, and its
 * whole look for a part a look keeps as Dark Legacy's. A lookup uploads on first use; a look
 * makes every lookup it needs as it opens. */
class DarkLegacyHudArt {
public:
    virtual ~DarkLegacyHudArt() = default;
    /** STATIC's picture `name`, the very texture the boxes draw with; null when there is none. */
    virtual const Texture* staticArt(std::string_view name) = 0;
    /** SELECT's picture `name`, the same way. */
    virtual const Texture* selectArt(std::string_view name) = 0;
    /** A pickup count's icon: STATIC's, else the level's own. */
    virtual const Texture* countArt(std::string_view icon) = 0;
    /** INVENTORY's picture `name`, such as a powerup's SHP_ picture. */
    virtual const Texture* inventoryArt(std::string_view name) = 0;
    /** The small capitals the boxes' levels and the help messages are written in. */
    virtual const TextPainter& smallCaps() const = 0;
    /** Dark Legacy's own look, for a part another look draws as it does. */
    virtual HudPainter& look() = 0;
};

} // namespace gdl::game
