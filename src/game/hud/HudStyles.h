#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl {
class RenderDevice;
}

namespace gdl::game {

class DarkLegacyHudArt;
class HudPainter;

/** A HUD look an optional expansion offers. */
struct HudStyleOffer {
    std::string id;      ///< as the settings name it; never kDarkLegacyHudStyle
    std::string label;   ///< the text id of its name
    bool usable = false; ///< whether this installation holds every file the look needs

    bool operator==(const HudStyleOffer&) const = default;
};

/** A party member whose class art a look takes as it opens. */
struct HudMember {
    s32 seat = -1;      ///< the seat whose box it is drawn in
    s32 classIndex = 0; ///< its character's class
    s32 color = 0;      ///< its costume colour

    bool operator==(const HudMember&) const = default;
};

/** The looks besides Dark Legacy's that the optional expansions offer, and their painters.
 * Nothing is looked for until asked. */
class HudStyles {
public:
    virtual ~HudStyles() = default;
    /** The looks offered, usable on this installation or not, in the order the HUD page lists
     * them; each ask looks on disk afresh. */
    virtual std::span<const HudStyleOffer> offers() = 0;
    /** Opens look `id`'s painter for `party`, taking its own art and the art it borrows from
     * Dark Legacy's now; null, with one log line from the opener, when it cannot. */
    virtual std::unique_ptr<HudPainter> open(std::string_view id, RenderDevice& device,
                                             DarkLegacyHudArt& darkLegacy,
                                             std::span<const HudMember> party) = 0;
};

} // namespace gdl::game
