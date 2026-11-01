#pragma once

#include "engine/core/Types.h"

namespace gdl::game::effect_flags {

/**
 * The flag word of an authored effect record (a class's or a critter's SFXX row), read by
 * whatever starts the effect: where it is hung, whether it follows the body, and what it
 * does besides show. The data names none of these; each is named for what the engine does
 * with it.
 */
inline constexpr u32 kBodyParent = 0x1;     ///< hung from the body's root node
inline constexpr u32 kShakes = 0x2;         ///< shakes the camera
inline constexpr u32 kOnFloor = 0x10;       ///< placed on the floor under its point
inline constexpr u32 kArenaCue = 0x20;      ///< invokes the current boss arena's callback
inline constexpr u32 kDetached = 0x40;      ///< placed once where its node is, then left there
inline constexpr u32 kAtInitialRoot = 0x80; ///< placed where the body first stood
inline constexpr u32 kSkin = 0x100;         ///< a texture sequence applied to the body itself
inline constexpr u32 kHidesBody = 0x400;    ///< the body is not drawn while it shows
inline constexpr u32 kRootParent = 0x800;   ///< hung from the root node's parent
inline constexpr u32 kFollows =
    kBodyParent | kRootParent;                    ///< rides on the body rather than staying put
inline constexpr u32 kOtherParent = 0x2000;       ///< hung from another actor: a move's partner
inline constexpr u32 kTrail = 0x4000;             ///< a missile's trail
inline constexpr u32 kShrinks = 0x10000;          ///< shrinks away as it ends
inline constexpr u32 kLeavesGenerator = 0x20000;  ///< what it shoots leaves a generator behind
inline constexpr u32 kUntilNextMove = 0x40000;    ///< removed when its owner's move changes
inline constexpr u32 kSummonsEnemies = 0x400000;  ///< what it shoots summons enemies where it lands
inline constexpr u32 kNoLoop = 0x800000;          ///< timed effects hold their final pose
inline constexpr u32 kParticleMask = 0x0F000000;  ///< a particle emitter rather than a tree: which
inline constexpr u32 kShortParticle = 0x02000000; ///< the emitter whose particles live six frames

} // namespace gdl::game::effect_flags
