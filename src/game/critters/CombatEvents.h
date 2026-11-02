#pragma once
#include <optional>
#include <string>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"

#include "game/critters/CombatantKind.h"
namespace gdl::game {
/** Physical contact adds velocity without dealing damage or selecting a hit reaction. */
struct CombatPush {
    s32 player = -1;
    Vec3 velocity{0};
    bool operator==(const CombatPush&) const = default;
};
/** A golem's uncredited contact damage to a small swarm enemy. */
struct CombatTrample {
    s32 enemy = -1;
    f32 damage = 0;
    Vec3 position{0};
    bool operator==(const CombatTrample&) const = default;
};
/** Hold updates carry an animated attachment; release carries the authored throw force.
 * Damage is deferred until the thrown player reaches the floor. A zero force cancels. */
struct CombatGrab {
    s32 player = -1;
    s32 critter = -1;
    std::optional<Mat4> attachment;
    Vec3 velocity{0};
    f32 damage = 0;
    bool restoreSupport = false; ///< release returns to the pre-parent support footprint
    bool operator==(const CombatGrab&) const = default;
};
/** A successful eruption makes its obstacle solid after the visual wind-up. */
struct CombatArenaActivation {
    usize index = 0;
    f32 delay = 0;
    bool operator==(const CombatArenaActivation&) const = default;
};

/** A contact a critter has made. Breath contacts repeat while touching; the
 * recipient's shared breath timer decides when they may damage it again. */
struct CombatBlow {
    s32 player = -1;
    s32 critter = -1;
    f32 damage = 0.0f;
    Vec3 direction{0.0f, 0.0f, 1.0f};
    bool breath = false;
    bool gated = false; ///< held off by the player's shared quarter-second hit gap (fxhittime)
    u32 flags = 0;      ///< authored player damage modifiers, not the attack's behavior flags
    Vec3 origin{0.0f};  ///< emitted segment origin, used for breath cover queries
    bool area = false;
    f32 repeatGap = 0.0f; ///< area-effect immunity requested on contact
    bool operator==(const CombatBlow&) const = default;
};

/** An effect and a sound a critter has set off: a move's, a strike's or a hit's, where it
 * happened; `follows` for one that rides on the body. */
struct CombatCue {
    s32 critter = -1;
    std::string tree;  ///< of the critter's own archive; empty for a sound alone
    std::string sound; ///< empty for an effect alone
    /** Whether the sound fades with the players' distance: a death's does not (CritterDoSfx
     * plays it through sndFxPlay3D rather than sndFxPlay3DAtten). */
    bool attenuated = true;
    /** CritterDoSfx sounds at the creature's root, independently of the effect's attachment. */
    Vec3 soundPosition{0.0f};
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
    f32 life = 0.0f; ///< seconds, when it does not play out
    bool follows = false;
    bool shakes = false;
    bool arena = false;              ///< stage callback, even when both tree and sound are empty
    bool untilNextMove = false;      ///< persistent cue cancelled by the next move transition
    bool stopMoveEffect = false;     ///< cancel the owner's previous untilNextMove cue
    bool rootAttachment = false;     ///< root transform, not a fixed world-space body offset
    std::optional<std::string> node; ///< animated attachment, distinct from a body translation
    Vec3 nodeOffset{0.0f};
    Vec2 pitchYaw{0.0f}; ///< local effect rotation, independent of its attachment offset
    bool loop = true;
    std::optional<Mat4> placement;       ///< complete world placement for a static arena effect
    std::optional<s32> playerAttachment; ///< player input id, independent of the captor's pose
    bool operator==(const CombatCue&) const = default;
};

/** A dying critter's death throwing something out (the coins a boss spews): from where,
 * which way and how fast, and how far round each side of that way. */
struct CombatSpew {
    s32 critter = -1;
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    f32 halfAngle = 0.0f; ///< radians
    bool operator==(const CombatSpew&) const = default;
};

/** Experience a critter is worth: a share of its value for each hit, to the hitter, and
 * a fifth of it to everyone (`player` -1) when it falls. */
struct CombatLoss {
    s32 critter = -1;
    CombatantKind kind = CombatantKind::Unknown;
    std::string form; ///< a gargoyle's ("EAGL"): the key it drops is named by it
    s32 player = -1;
    f32 experience = 0.0f;
    bool killed = false;
    bool drop = false; ///< animation finished; release the carried item at the final position
    Vec3 position{0.0f, 0.0f, 0.0f};
    bool operator==(const CombatLoss&) const = default;
};

inline void visitNativeState(NativeStateDigest& digest, const CombatPush& state) {
    digest.fields<2>(state);
}
inline void visitNativeState(NativeStateDigest& digest, const CombatTrample& state) {
    digest.fields<3>(state);
}
inline void visitNativeState(NativeStateDigest& digest, const CombatGrab& state) {
    digest.fields<6>(state);
}
inline void visitNativeState(NativeStateDigest& digest, const CombatArenaActivation& state) {
    digest.fields<2>(state);
}
inline void visitNativeState(NativeStateDigest& digest, const CombatBlow& state) {
    digest.fields<10>(state);
}
inline void visitNativeState(NativeStateDigest& digest, const CombatCue& state) {
    digest.fields<21>(state);
}
inline void visitNativeState(NativeStateDigest& digest, const CombatSpew& state) {
    digest.fields<4>(state);
}
inline void visitNativeState(NativeStateDigest& digest, const CombatLoss& state) {
    digest.fields<8>(state);
}

} // namespace gdl::game
