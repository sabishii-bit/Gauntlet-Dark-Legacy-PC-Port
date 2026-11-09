#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/players/PlayerImpact.h"

namespace gdl::game {

/** A native barrel's single, duration-bound attack. Each victim is reached at
 * most once; an actor entering later may still be hit while the blast exists.
 * Flags and sound are native identifiers, not GDL attack flags or audio keys. */
struct LegendsFixtureEvent {
    enum class Kind { Explosion, Poison };
    Kind kind = Kind::Explosion;
    u32 nativeIndex = 0;
    Vec3 position{0};
    f32 radius = 0;
    f32 damage = 0;
    u32 flags = 0;
    u32 sound = 0;
    s32 player = -1;
    f32 seconds = 0;
    f32 growFraction = .1f;
    f32 shrinkFraction = .2f;
    bool noFalloff = false;
};

/** A native trap's blow on one player. The level applies it through the
 * ordinary player combat pipeline, with its platform's armor rule. */
struct LegendsTrapHit {
    u32 nativeIndex = 0;
    u8 player = 0;
    f32 damage = 0;
    bool pierces = false;
    Vec3 position{0};
    PlayerImpact impact;
    u32 sound = 0;
};

// Native e007a25c (plates, gates and traps) and e007a4a0 (retained moving
// scenery) take no world position. Their authored macro gain/pan must not be
// faded by an invented distance curve. IPR ambient emitters use their separate
// native radius controller; positional fixture routing must be explicit.
enum class LegendsSoundRoute { Global, Positional };

struct LegendsSoundEvent {
    u32 token = 0;
    Vec3 position{0};
    LegendsSoundRoute route = LegendsSoundRoute::Global;
};

/** A retained native fixture voice. Identity belongs to the motion target,
 * not whichever plate requested its current endpoint. */
struct LegendsSoundLoop {
    u32 id = 0;
    u32 token = 0;
    Vec3 position{0};
    LegendsSoundRoute route = LegendsSoundRoute::Global;
};

} // namespace gdl::game
