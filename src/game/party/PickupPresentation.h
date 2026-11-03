#pragma once
#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {
class PlayerActor;

/** What the party's pickups put up as they are taken: the taker's card, a count over a
 * player's box, the selector turned to a powerup just taken and the bosses' keys. Answered at
 * once; kept by nobody. */
class PickupPresentation {
public:
    virtual ~PickupPresentation() = default;
    /** Card `texture` sliding up over the box of the player at input slot `seat`. */
    virtual void addCard(s32 seat, std::string_view texture) = 0;
    /** `count` of `total` beside `icon` over the box of the player at input slot `seat`. */
    virtual void showCount(s32 seat, std::string_view icon, s32 count, s32 total) = 0;
    /** `actor`'s selector turned to the powerup of `kind` and `flags` it has just taken. */
    virtual void focusPickup(const PlayerActor& actor, s32 kind, u32 flags) = 0;
    /** The bosses' keys shown in the boxes a while. */
    virtual void showRelics() = 0;
};
} // namespace gdl::game
