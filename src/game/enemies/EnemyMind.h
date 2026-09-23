#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/EnemyAnimator.h"

namespace gdl::game {

/** What an enemy remembers between ticks for its mind's sake. */
struct MindMemory {
    f32 heading = 0.0f;       ///< the way it means to go
    f32 headingBefore = 0.0f; ///< the way it meant to go before that
    s32 route = 0;            ///< which way round a corner: negative left, positive right
    s32 collided = 0;         ///< bumps since the route was chosen
    s32 stuck = 0;            ///< headings refused in a row
    s32 deadEnd = 0;          ///< ticks a heading is held after a bump
    s32 turns = 0;            ///< turns taken at dead ends
    s32 counter = 0;          ///< whatever a mind counts
    s32 mode = 0;             ///< which stage of its business a mind is at
    s32 fuse = 0;             ///< ticks before a suicide's fuse is lit, or a thrower's next
    bool woken = false;       ///< a lurker has seen someone
    bool skirting = false;    ///< a chaser is going round something
    bool keepingOff = false;  ///< a skirmisher is backing away
};

/** What a mind senses of its body and the world in a tick. */
struct MindSense {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 yaw = 0.0f; ///< the way the body faces
    f32 radius = 1.0f;
    f32 sight = 30.0f;
    f32 pace = 0.1f; ///< how far a tick's step goes
    s32 ticks = 1;
    s32 target = -1; ///< the player it is after, if any
    Vec3 targetPosition{0.0f, 0.0f, 0.0f};
    f32 targetDistance = 100000.0f;
    bool recognized = false; ///< it has seen its player within sight
    s32 contact = -1;        ///< the player it stands against, if any
    Vec3 contactPosition{0.0f, 0.0f, 0.0f};
    bool bumpedWall = false;  ///< the last step ran into the world
    bool bumpedOther = false; ///< or into another enemy
    bool blocked = false;     ///< and came to a dead stop there
    s32 otherSide = 1;        ///< which way round the enemy it bumped is nearer
    bool generatorGone = false;
    f32 targetVertical = 0.0f;               ///< how far above the body its player stands
    bool threw = false;                      ///< the body let a missile go this tick
    s32 idleTicks = 120;                     ///< ticks a thrower waits between throws
    EnemyAction action = EnemyAction::Ready; ///< what the body is doing
    /** Whether a step along a heading crosses nothing (a body against a wall may still
     * slide along it). */
    std::function<bool(f32 heading)> clear;
    /** Whether a step along a heading keeps the whole body off the walls. */
    std::function<bool(f32 heading)> open;

    /** The way to the player, or the way the mind was going with no player to face. */
    f32 faceAngle(f32 fallback) const;
    /** Which way round the player is nearer: a sixth of a turn either side of the facing is
     * probed and the side that ends nearer chosen. */
    s32 nearerSide() const;
    bool clearAlong(f32 heading) const { return !clear || clear(heading); }
    bool openAlong(f32 heading) const { return !open || open(heading); }
};

/** What a mind decides for the tick. */
struct MindIntent {
    f32 heading = 0.0f;
    f32 pace = 1.0f;  ///< of the body's; nought stands still
    bool turn = true; ///< whether the body turns to the heading
    EnemyAction action = EnemyAction::Walk;
    std::optional<s32> become; ///< another way to go about from now on
    bool expire = false;       ///< the body is done with (a loiterer whose generator is gone)
    bool throwing = false;     ///< the body is asked to throw
    bool explode = false;      ///< the body blows itself up
};

/**
 * One way of going about: the original's `move_logic` handlers, each a mind of its own.
 * A mind only senses and decides; the body carries the decision out.
 */
class EnemyMind {
public:
    virtual ~EnemyMind() = default;
    virtual std::string_view name() const = 0;
    virtual MindIntent think(MindMemory& memory, const MindSense& sense) const = 0;
};

/** The mind that goes about the original's way number `algorithm`; a stranger's is the
 * wanderer's. */
const EnemyMind& enemyMindOf(s32 algorithm);

/** The ways with a mind of their own. */
inline constexpr s32 kSeekWay = 0;
inline constexpr s32 kProwlWay = 2;
inline constexpr s32 kWanderWay = 5;
inline constexpr s32 kWanderOtherWay = 6;
inline constexpr s32 kChaseWay = 7;
inline constexpr s32 kLoiterWay = 11;
inline constexpr s32 kSkirmishWay = 16; ///< the archer's: shooting from range, keeping it
inline constexpr s32 kBombWay = 17;     ///< the bomber's: standing and lobbing
inline constexpr s32 kSuicideWay = 18;  ///< the suicide's: a lit fuse and a run
inline constexpr s32 kThrowWay = 23;    ///< standing and shooting
inline constexpr s32 kFleeWay = 24;
inline constexpr s32 kLurkWay = 27;
inline constexpr s32 kStandWay = 31;

/** Angles wrapped into a half turn either way. */
f32 wrapAngle(f32 angle);

} // namespace gdl::game
