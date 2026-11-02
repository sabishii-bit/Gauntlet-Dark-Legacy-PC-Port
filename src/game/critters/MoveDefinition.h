#pragma once
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/EffectFlags.h"
#include "game/hud/HealthMeter.h"
namespace gdl::game {
/** CritterInitMoves/LoadFinish pass a 16-byte field width to AtreeFindNodeIdx,
 * which compares width minus one. ANIM node names can be longer than the WAD references. */
inline constexpr usize kCombatantNodeNameLength = 15;

/** When a move may be chosen against its target. */
struct TargetCriteria {
    f32 minDistance = 0.0f;
    f32 maxDistance = 0.0f; ///< none when nought or under
    f32 yaw = 0.0f;         ///< where the cone points, from ahead: behind for a turn about
    f32 minDot = -1.0f;     ///< how squarely the cone must hold the target
    f32 maxVertical = 0.0f; ///< none when nought
    f32 minRateScale = 0.0f;
    f32 maxRateScale = 0.0f;    ///< exclusive; uncapped when not above the minimum
    f32 maxHomeDistance = 0.0f; ///< WAD's idleGate: distance from the boss's home

    /** Whether a target `distance` away, `bearing` round from ahead and `vertical` above or
     * below, passes. */
    bool allows(f32 distance, f32 bearing, f32 vertical) const;
    bool allowsPhase(f32 rateScale, f32 homeDistance) const;
};

/** A gated sequence of moves. Its entry criteria do not apply again between steps. */
struct AttackPattern {
    static constexpr u32 kNeedsChildren = 0x2; ///< flags: only while the children are intact
    static constexpr u32 kDisabled = 0x1000;   ///< flags: never chosen

    u32 flags = 0;
    f32 cooldown = 0.0f;
    std::vector<s32> moves;
    TargetCriteria target;
};

/** How a move harms: a blow about a part of the body, a breath, or a ring about the feet. */
struct AttackDefinition {
    static constexpr s16 kBlow = 0;
    static constexpr s16 kProjectile = 1;
    static constexpr s16 kAttachedArea = 2;
    static constexpr s16 kRing = 3; ///< an expanding effect with fading damage
    static constexpr s16 kBreath = 4;
    static constexpr s16 kArenaAreas = 5;    ///< one effect at each authored arena anchor
    static constexpr s16 kArenaEruption = 6; ///< raise an inactive arena obstacle near the target
    static constexpr s16 kGrab = 7;
    static constexpr s16 kTargetArea = 8;  ///< effect at the move's saved player position
    static constexpr s16 kSpew = 9;        ///< a boss's death throwing its coins out
    static constexpr u32 kCurbed = 0x4000; ///< a legend item's weakness takes this from it

    s16 type = 0;
    u16 behaviorFlags = 0; ///< launch/attachment policy, distinct from player harm flags
    u32 flags = 0;
    f32 radius = 0.0f;
    f32 maxDistance = 0.0f;
    f32 minDistance = 0.0f; ///< inner horizontal limit of a damaging breath
    f32 yaw = 0.0f;         ///< a spew's way, turned from the body's facing
    f32 minDot = 0.0f;      ///< also the cosine of half a spew's arc
    f32 pitch = 0.0f;       ///< a spew's way, tipped (under nought: up)
    Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 damage = 0.0f;
    f32 speed = 0.0f; ///< a spew's, in units a second
    f32 maxSpeed = 0.0f;
    f32 gravity = 0.0f;
    f32 morphLife = 0.0f;
    f32 yawSpread = 0.0f;
    s32 sound = -1; ///< the sound record started where it strikes, or -1
    s32 hitSound = -1;
    s32 morph = -1;
    s32 morphEnd = -1;

    /** The way a spew goes from a body facing `yaw` (radians about the upright), and how
     * fast; half its arc each side of that is `acos(minDot)`. */
    Vec3 spewVelocity(f32 bodyYaw) const;
    f32 spewHalfAngle() const;
};

/** An effect and a sound a critter's move, strike or hurt starts: the tree of its own
 * archive, the sound named with the level's letter, and how it is placed. */
struct CombatEffectDefinition {
    std::string tree;        ///< "ATK01FX"; "NULLFX" or empty shows nothing
    std::string soundFormat; ///< "S_GOL%cSWING"
    u32 flags = 0;           ///< effect_flags::
    s32 link = -1;           ///< another started with it
    Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 life = 0.0f;
    f32 scale = 1.0f;

    f32 particleRate = 0.0f;  ///< custom emitter births per 30 Hz frame
    f32 particleSpeed = 0.0f; ///< custom emitter speed in world units per second
    s32 skinLoops = 0;        ///< additional passes of a skin sequence (SFXX custom0)
    f32 impactRadius = 0; ///< adapter-supplied impact radius; zero retains ordinary GDL behavior
    f32 impactPhysicalFraction =
        1; ///< native split retained for adapters; common hits are combined
    f32 impactElementalFraction = 0;
    u32 impactElement = 0;

    bool shows() const { return !tree.empty() && tree != "NULLFX"; }
    bool follows() const { return (flags & effect_flags::kFollows) != 0; }
    /** The sound's name for a level whose name starts with `letter`. */
    std::string soundFor(char letter) const;
};

/** One thing a critter does, and when it does it. */
struct MoveDefinition {
    static constexpr u32 kNeedsChildren = 0x2; ///< flags: only while the children are intact
    static constexpr u32 kLinkedOnly = 0x4;    ///< flags: never chosen, only reached as a link
    static constexpr u32 kNeedsNode = 0x10;    ///< flags: only while its node is there
    static constexpr s32 kInit = 0;
    static constexpr s32 kStart = 16;
    static constexpr s32 kDeath = 17;
    static constexpr s32 kReady = 32;
    static constexpr s32 kTaunt = 33;
    static constexpr s32 kRoar = 34;
    static constexpr s32 kBlock = 35;
    static constexpr s32 kStepFrom = 48; ///< the steps: turns, walks, back-steps
    static constexpr s32 kStepLeft = 50;
    static constexpr s32 kStepRight = 51;
    static constexpr s32 kWalk = 52;
    static constexpr s32 kStepBack = 53;
    static constexpr s32 kStepToPoint = 56;
    static constexpr s32 kStepLast = 57; ///< the last of the walk family (MOVE_STEPLAST)
    static constexpr s32 kStepTo = 64;   ///< the steps run up to here
    static constexpr s32 kHitReact = 64; ///< a flinch from a hit flagged to shake it
    static constexpr s32 kKnockBack = 65;
    static constexpr s32 kKnockDown = 66;
    static constexpr s32 kAttackFrom = 128; ///< attacks are this and over
    static constexpr s32 kGrab = 129;
    static constexpr s32 kTargetArea = 136; ///< snapshots the target before emitting its effects
    static constexpr s32 kCutsIn = 0xF00; ///< bypasses priority comparison, not interrupt immunity

    s32 type = 0;
    u32 flags = 0;
    s32 priority = 0;
    std::string name;
    std::string anim;
    std::string colnode;
    s32 frameStart = -1;
    s32 frameEnd = -1;
    s32 frameStart2 = -1;
    s32 frameEnd2 = -1;
    f32 framePeriod = 0.0f; ///< repeat spacing for move type 133
    s32 damage0 = -1;
    s32 damage1 = -1;
    s32 link = -1;
    s32 interrupt = 0;
    s32 sound = -1; ///< the sound record started as the move passes `soundFrame`
    s32 soundFrame = 0;
    s32 sound2 = -1; ///< and a second, at `sound2Frame`
    s32 sound2Frame = 0;
    TargetCriteria target;
    f32 cooldown = 0.0f;
    f32 speed = 0.0f;    ///< units a second while it plays
    f32 turnRate = 0.0f; ///< radians a second
    f32 hold = 0.0f;

    bool attack() const { return type >= kAttackFrom && type < 240; }
    bool reaction() const { return type >= kHitReact && type < kAttackFrom; }
    bool interrupts(const MoveDefinition& current) const;
    bool harms() const { return damage0 >= 0 || damage1 >= 0; }
    /** Projectile triggers crossed between integer animation frames; -1 precedes frame zero. */
    s32 projectileTriggers(s32 previous, s32 current, bool second = false) const;
};

/** A node turned to the creature's target as it acts: the head, or the eyes
 * (CritterLookAtPlayer's hitnode0 and hitnode1, TYPE +0x20/+0x30 and +0x60..+0x74). */
struct LookDefinition {
    std::string node;     ///< empty: none
    bool parent = false;  ///< the node's parent is turned instead (TYPE flag 0x10, the head only)
    f32 yawRate = 0.0f;   ///< radians it may be turned from its animation, about the upright
    f32 pitchRate = 0.0f; ///< and up or down
    f32 pitchBias = 0.0f; ///< pitch added to the way it looks
    bool turns() const { return !node.empty() && (yawRate > 0.0f || pitchRate > 0.0f); }
};

/** A part of the body that can be struck. */
struct CritterPart {
    std::string node;
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;
    f32 damageScale = 1.0f;
    f32 healthScale = 1.0f;
    s32 damageEffect = -1; ///< DAMG record emitted when this part breaks
    u32 flags = 0;
    f32 targetScoreScale = 0.0f;  ///< NODE aim preference; zero disables assisted selection
    f32 maxTargetDistance = 0.0f; ///< zero leaves the caller's range in force
    static constexpr u32 kBreakable = 2;
    static constexpr u32 kRemoveChildren = 4;
    static constexpr u32 kSolid = 8; ///< stops movement, independently of accepting damage
};

} // namespace gdl::game
