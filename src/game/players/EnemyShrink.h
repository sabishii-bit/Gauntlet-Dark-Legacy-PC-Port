#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * The enemy shrinker (special 0x200), the original's per-frame scale on the swarm and the
 * great ones (SetPlayerVars, gamemain.c 1576): every standing wearer shrinks them by two
 * thirds again, only where no boss is fought. Shrunk, they show and cast their shadows at that
 * size, take twice the harm (damage_enemy, CritterDamage) and deal half of it (the blows,
 * their missiles and the great ones' contacts).
 */
struct EnemyShrink {
    static constexpr f32 kPerWearer = 0.667f;
    static constexpr f32 kHarmTaken = 2.0f;
    static constexpr f32 kHarmDealt = 0.5f;
    static constexpr f32 kWhole = 1.0f;
    /** Heard as the scale rises back (fn_8009D530). */
    static constexpr std::string_view kUnshrinkSound = "S_UNSHRINK";

    /** The scale `wearers` standing shrinkers leave the swarm at. */
    static f32 scaleOf(s32 wearers, bool bossEncounter);
    static bool shrunk(f32 scale) { return scale < kWhole; }
    /** What one of the shrunk takes of `amount`. */
    static f32 harmTaken(f32 scale, f32 amount) {
        return shrunk(scale) ? amount * kHarmTaken : amount;
    }
    /** What one of the shrunk deals of `amount`. */
    static f32 harmDealt(f32 scale, f32 amount) {
        return shrunk(scale) ? amount * kHarmDealt : amount;
    }
    /** Whether the scale rising from `before` to `now` sounds the unshrink. */
    static bool rises(f32 before, f32 now) { return now > before; }
};

} // namespace gdl::game
