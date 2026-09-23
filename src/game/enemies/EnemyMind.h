#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "engine/math/Math.h"

#include "game/enemies/EnemyAnimator.h"

namespace gdl::game {

/** What an enemy remembers between ticks for its mind's sake. */
struct MindMemory {
    float heading = 0.0f;       ///< the way it means to go
    float headingBefore = 0.0f; ///< the way it meant to go before that
    std::int32_t route = 0;     ///< which way round a corner: negative left, positive right
    std::int32_t collided = 0;  ///< bumps since the route was chosen
    std::int32_t stuck = 0;     ///< headings refused in a row
    std::int32_t deadEnd = 0;   ///< ticks a heading is held after a bump
    std::int32_t turns = 0;     ///< turns taken at dead ends
    std::int32_t counter = 0;   ///< whatever a mind counts
    std::int32_t mode = 0;      ///< which stage of its business a mind is at
    std::int32_t fuse = 0;      ///< ticks before a suicide's fuse is lit, or a thrower's next
    bool woken = false;         ///< a lurker has seen someone
    bool skirting = false;      ///< a chaser is going round something
    bool keepingOff = false;    ///< a skirmisher is backing away
};

/** What a mind senses of its body and the world in a tick. */
struct MindSense {
    Vec3 position{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f; ///< the way the body faces
    float radius = 1.0f;
    float sight = 30.0f;
    float pace = 0.1f; ///< how far a tick's step goes
    std::int32_t ticks = 1;
    std::int32_t target = -1; ///< the player it is after, if any
    Vec3 targetPosition{0.0f, 0.0f, 0.0f};
    float targetDistance = 100000.0f;
    bool recognized = false;   ///< it has seen its player within sight
    std::int32_t contact = -1; ///< the player it stands against, if any
    Vec3 contactPosition{0.0f, 0.0f, 0.0f};
    bool bumpedWall = false;    ///< the last step ran into the world
    bool bumpedOther = false;   ///< or into another enemy
    bool blocked = false;       ///< and came to a dead stop there
    std::int32_t otherSide = 1; ///< which way round the enemy it bumped is nearer
    bool generatorGone = false;
    float targetVertical = 0.0f;             ///< how far above the body its player stands
    bool threw = false;                      ///< the body let a missile go this tick
    std::int32_t idleTicks = 120;            ///< ticks a thrower waits between throws
    EnemyAction action = EnemyAction::Ready; ///< what the body is doing
    /** Whether a step along a heading crosses nothing (a body against a wall may still
     * slide along it). */
    std::function<bool(float heading)> clear;
    /** Whether a step along a heading keeps the whole body off the walls. */
    std::function<bool(float heading)> open;

    /** The way to the player, or the way the mind was going with no player to face. */
    float faceAngle(float fallback) const;
    /** Which way round the player is nearer: a sixth of a turn either side of the facing is
     * probed and the side that ends nearer chosen. */
    std::int32_t nearerSide() const;
    bool clearAlong(float heading) const { return !clear || clear(heading); }
    bool openAlong(float heading) const { return !open || open(heading); }
};

/** What a mind decides for the tick. */
struct MindIntent {
    float heading = 0.0f;
    float pace = 1.0f; ///< of the body's; nought stands still
    bool turn = true;  ///< whether the body turns to the heading
    EnemyAction action = EnemyAction::Walk;
    std::optional<std::int32_t> become; ///< another way to go about from now on
    bool expire = false;   ///< the body is done with (a loiterer whose generator is gone)
    bool throwing = false; ///< the body is asked to throw
    bool explode = false;  ///< the body blows itself up
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
const EnemyMind& enemyMindOf(std::int32_t algorithm);

/** The ways with a mind of their own. */
inline constexpr std::int32_t kSeekWay = 0;
inline constexpr std::int32_t kProwlWay = 2;
inline constexpr std::int32_t kWanderWay = 5;
inline constexpr std::int32_t kWanderOtherWay = 6;
inline constexpr std::int32_t kChaseWay = 7;
inline constexpr std::int32_t kLoiterWay = 11;
inline constexpr std::int32_t kSkirmishWay = 16; ///< the archer's: shooting from range, keeping it
inline constexpr std::int32_t kBombWay = 17;     ///< the bomber's: standing and lobbing
inline constexpr std::int32_t kSuicideWay = 18;  ///< the suicide's: a lit fuse and a run
inline constexpr std::int32_t kThrowWay = 23;    ///< standing and shooting
inline constexpr std::int32_t kFleeWay = 24;
inline constexpr std::int32_t kLurkWay = 27;
inline constexpr std::int32_t kStandWay = 31;

/** Angles wrapped into a half turn either way. */
float wrapAngle(float angle);

} // namespace gdl::game
