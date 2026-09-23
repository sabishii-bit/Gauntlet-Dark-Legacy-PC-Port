#pragma once

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
    int route = 0;              ///< which way round a corner: negative left, positive right
    int collided = 0;           ///< bumps since the route was chosen
    int stuck = 0;              ///< headings refused in a row
    int deadEnd = 0;            ///< ticks a heading is held after a bump
    int turns = 0;              ///< turns taken at dead ends
    int counter = 0;            ///< whatever a mind counts
    int mode = 0;               ///< which stage of its business a mind is at
    int fuse = 0;               ///< ticks before a suicide's fuse is lit, or a thrower's next
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
    int ticks = 1;
    int target = -1; ///< the player it is after, if any
    Vec3 targetPosition{0.0f, 0.0f, 0.0f};
    float targetDistance = 100000.0f;
    bool recognized = false; ///< it has seen its player within sight
    int contact = -1;        ///< the player it stands against, if any
    Vec3 contactPosition{0.0f, 0.0f, 0.0f};
    bool bumpedWall = false;  ///< the last step ran into the world
    bool bumpedOther = false; ///< or into another enemy
    bool blocked = false;     ///< and came to a dead stop there
    int otherSide = 1;        ///< which way round the enemy it bumped is nearer
    bool generatorGone = false;
    float targetVertical = 0.0f;             ///< how far above the body its player stands
    bool threw = false;                      ///< the body let a missile go this tick
    int idleTicks = 120;                     ///< ticks a thrower waits between throws
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
    int nearerSide() const;
    bool clearAlong(float heading) const { return !clear || clear(heading); }
    bool openAlong(float heading) const { return !open || open(heading); }
};

/** What a mind decides for the tick. */
struct MindIntent {
    float heading = 0.0f;
    float pace = 1.0f; ///< of the body's; nought stands still
    bool turn = true;  ///< whether the body turns to the heading
    EnemyAction action = EnemyAction::Walk;
    std::optional<int> become; ///< another way to go about from now on
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
const EnemyMind& enemyMindOf(int algorithm);

/** The ways with a mind of their own. */
inline constexpr int kSeekWay = 0;
inline constexpr int kProwlWay = 2;
inline constexpr int kWanderWay = 5;
inline constexpr int kWanderOtherWay = 6;
inline constexpr int kChaseWay = 7;
inline constexpr int kLoiterWay = 11;
inline constexpr int kSkirmishWay = 16; ///< the archer's: shooting from range, keeping it
inline constexpr int kBombWay = 17;     ///< the bomber's: standing and lobbing
inline constexpr int kSuicideWay = 18;  ///< the suicide's: a lit fuse and a run
inline constexpr int kThrowWay = 23;    ///< standing and shooting
inline constexpr int kFleeWay = 24;
inline constexpr int kLurkWay = 27;
inline constexpr int kStandWay = 31;

/** Angles wrapped into a half turn either way. */
float wrapAngle(float angle);

} // namespace gdl::game
