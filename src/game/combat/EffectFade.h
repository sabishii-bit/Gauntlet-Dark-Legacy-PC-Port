#pragma once

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game::effect_fade {

/**
 * How long one of the world's effects holds what it harms over two from the effects that share
 * its wait: ProcessEffects' fade (sfx.c 2677-2716). The effect's age is its owner's to tell:
 * what is left of a spreading one and a fifteenth (a steady one's up to a second), a flying
 * one's hit effect's frames over thirty, or `kShot` without them.
 */
inline constexpr f32 kShot = 0.25f;       ///< a flying effect's age without a hit effect
inline constexpr f32 kLeast = 0.2f;       ///< the least fade of one that does not spread
inline constexpr f32 kSpreadLeast = 1.0f; ///< a spreading effect's (0x20) is never shorter
inline constexpr f32 kPierce = 3.0f;      ///< DMG_SUPER holds this past the effect's age

/** A spreading effect's fade, from its age; `pierces` for one of DMG_SUPER. */
constexpr f32 spreading(f32 age, bool pierces) {
    return pierces ? kPierce + age : std::max(kSpreadLeast, age);
}

/** A flying effect's fade, from its age; `pierces` for one of DMG_SUPER, whose age stays a
 * shot's. */
constexpr f32 flying(f32 age, bool pierces) {
    return pierces ? kPierce + kShot : std::max(kLeast, age);
}

} // namespace gdl::game::effect_fade
