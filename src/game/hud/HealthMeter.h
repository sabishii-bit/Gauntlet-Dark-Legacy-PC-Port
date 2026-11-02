#pragma once
#include <string>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** How a boss's health meter is laid out across the top of the screen: strips of 256, the
 * first's cap and the last's taken off the fill. A critter's archive says it; the HUD draws
 * it. */
struct HealthMeterDefinition {
    static constexpr u32 kShown = 4;       ///< the type flag for a HUD meter
    static constexpr u32 kBacked = 8;      ///< and for its backgrounds
    static constexpr u32 kInWorld = 0x800; ///< the bar that hangs off the body

    s32 pieces = 0;
    s32 advance = 0;
    s32 leftInset = 0;
    s32 rightInset = 0;
    bool shown = false;
    bool backed = false;
    bool inWorld = false; ///< the GMETER bar hangs over the body
    std::string name;     ///< TYPE suffix; empty for an ordinary boss, EAGLE/LION/SNAKE for heads
    Vec3 barOffset{0.0f, 0.0f, 0.0f}; ///< where the in-world bar hangs
};

/** One independently damaged body's HUD meter, in stable root/child order. */
struct HealthMeterReading {
    HealthMeterDefinition definition;
    f32 health = 0;
    f32 maxHealth = 1;
};

} // namespace gdl::game
