#pragma once
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
namespace gdl::game {
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
    static constexpr u32 kFollows = 0x801;         ///< rides on the body rather than staying put
    static constexpr u32 kShakes = 0x2;            ///< shakes the camera
    static constexpr u32 kArenaCue = 0x20;         ///< invokes the current boss arena's callback
    static constexpr u32 kUntilNextMove = 0x40000; ///< removed when its owner's move changes

    std::string tree;        ///< "ATK01FX"; "NULLFX" or empty shows nothing
    std::string soundFormat; ///< "S_GOL%cSWING"
    u32 flags = 0;
    s32 link = -1; ///< another started with it
    Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 life = 0.0f;
    f32 scale = 1.0f;

    bool shows() const { return !tree.empty() && tree != "NULLFX"; }
    bool follows() const { return (flags & kFollows) != 0; }
    /** The sound's name for a level whose name starts with `letter`. */
    std::string soundFor(char letter) const;
};

/** One thing a critter does, and when it does it. */
struct MoveDefinition {
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
    static constexpr s32 kStepTo = 64;
    static constexpr s32 kKnockBack = 65;
    static constexpr s32 kKnockDown = 66;
    static constexpr s32 kAttackFrom = 128; ///< attacks are this and over
    static constexpr s32 kTargetArea = 136; ///< snapshots the target before emitting its effects

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
    bool interrupts(const MoveDefinition& current) const;
    bool harms() const { return damage0 >= 0 || damage1 >= 0; }
    /** Projectile triggers crossed between integer animation frames; -1 precedes frame zero. */
    s32 projectileTriggers(s32 previous, s32 current, bool second = false) const;
};

/** A part of the body that can be struck. */
struct CritterPart {
    std::string node;
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;
    f32 damageScale = 1.0f;
};

/** How a boss's health meter is laid out across the top of the screen: strips of 256, the
 * first's cap and the last's taken off the fill. */
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
    Vec3 barOffset{0.0f, 0.0f, 0.0f}; ///< where the in-world bar hangs
};

} // namespace gdl::game
