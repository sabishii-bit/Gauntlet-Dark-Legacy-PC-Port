#include "game/enemies/EnemyMind.h"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr f32 kHeadingSlack = 0.0349f; ///< two degrees: a heading this close is the same
constexpr s32 kStuckTurns = 10;        ///< probes refused before going straight anyway
constexpr s32 kCornerTurns = 7;        ///< bumps before the route doubles the other way
constexpr s32 kShortWait = 10;         ///< ticks a heading is held after a bump
constexpr s32 kLongWait = 60;
constexpr s32 kShortWaitOther = 15; ///< when the bump was another enemy
constexpr s32 kLongWaitOther = 50;
constexpr f32 kWanderTurn = kPi / 2.0f; ///< a quarter turn at a blocked probe or an expired hold
constexpr s32 kWanderWait = 20;
constexpr s32 kWanderCycle = 4;
constexpr s32 kSeekWait = 5;
constexpr s32 kSeekReverseStops = 9;
constexpr f32 kProwlTurn = kPi / 4.0f;
constexpr s32 kProwlWait = 30;
constexpr f32 kProwlPounce = 8.0f;        ///< a prowler goes for a player this close
constexpr s32 kProwlTurns = 4;            ///< turns at stops before it turns the other way
constexpr f32 kLoiterTurn = kPi / 180.0f; ///< a tick: a sixth of a turn a second (do_ai way 11)
constexpr f32 kThrowReach = 10.0f;        ///< a thrower's player must be within this above or below
constexpr f32 kKeepOffFrom = 0.6f;        ///< of its sight, a skirmisher backs off from
constexpr f32 kKeepOffTo = 0.8f;          ///< of its sight, and stops at
constexpr f32 kKeepOffPace = 0.8f;
constexpr s32 kFuseTicks = 60;
constexpr s32 kBurnTicks = 240; ///< a suicide's run before it blows up anyway
constexpr f32 kSuicidePace = 1.5f;
constexpr f32 kLungeFrom = 10.0f; ///< a lunger within this lunges, else makes its power attack
constexpr f32 kLungeLands = 7.5f; ///< and its lunge lands on a player within this
constexpr s32 kLungeWait = 30;    ///< ticks between attacks, and as many again at random
constexpr f32 kCreepPace = 0.5f;
constexpr f32 kFleePace = 2.0f;
constexpr f32 kHandToHand = 6.0f;         ///< a ranged caster this near its player chases it
constexpr s32 kFirstWaitSpread = 30;      ///< a ranged caster's first wait, in ticks, at random
constexpr s32 kFirstThrowWaitSpread = 10; ///< archers and bombers' entry hold
constexpr f32 kCastReach = 10.0f;         ///< and it casts only at a player this far above or below
constexpr f32 kBackOffWithin = 8.0f;      ///< a range keeper backs off from within this
constexpr f32 kBackedOff = 10.0f;         ///< until beyond this
constexpr f32 kCloseInBeyond = 18.0f;     ///< and closes in from beyond this
constexpr f32 kClosedIn = 16.0f;          ///< until within this
constexpr f32 kRangePace = 0.8f;
constexpr f32 kZigZagCloseIn = 8.0f;          ///< a zig-zagger this near its player seeks it
constexpr s32 kSwingTicks = 45;               ///< between a zig-zagger's quarter-turn swings
constexpr s32 kSwingsBeforeAim = 4;           ///< and how many it makes before it may aim afresh
constexpr s32 kAimHold = 30;                  ///< ticks after a fresh aim before another
constexpr f32 kAimOffset = kPi / 4.0f;        ///< off the player a fresh aim goes
constexpr f32 kAimOffsetGrowth = kPi / 12.0f; ///< and further for each aim still counted
constexpr f32 kPatrolNotice = 0.8f; ///< of its sight, a patroller leaves off for its player
constexpr f32 kLookoutReached = 1.0f;
constexpr f32 kLookoutHeight = 4.0f;
constexpr f32 kCastCloseIn = 6.0f; ///< a caster this near its player fights it hand to hand
constexpr s32 kCastLeast = 20;     ///< ticks a cast is asked for, and as many as ten more
constexpr s32 kCastSpread = 10;
constexpr s32 kCastFirstWait = 60; ///< the moving caster's initial hold, plus up to as many ticks
/** A fleer's nudges off straight away, one more for each step in a row that gets nowhere:
 * five degrees either side, then ten, to twenty (lbl_8011BF60's first eight). */
constexpr std::array<f32, 8> kFleeNudges{0.0872664601f, -0.0872664601f, 0.17453292f, -0.17453292f,
                                         0.261799395f,  -0.261799395f,  0.34906584f, -0.34906584f};

// The corner-hugging offsets, one more sixteenth of a turn for every bump.
constexpr std::array<f32, 8> kCornerOffsets{
    0.0f,       kPi / 8.0f,        kPi / 4.0f,        3.0f * kPi / 8.0f,
    kPi / 2.0f, 5.0f * kPi / 8.0f, 3.0f * kPi / 4.0f, 7.0f * kPi / 8.0f};

f32 yawBetween(const Vec3& from, const Vec3& to) {
    return std::atan2(to.x - from.x, to.z - from.z);
}

f32 flatDistance(const Vec3& a, const Vec3& b) {
    return glm::length(Vec2{a.x - b.x, a.z - b.z});
}

/** A hold on the heading, unless one is already running. */
void hold(MindMemory& memory, s32 ticks) {
    if (memory.deadEnd <= 0) {
        memory.deadEnd = ticks;
    }
}

/** The bookkeeping a bump does for the corner-huggers: a short hold and one more bump on
 * the route, or, on a route already doubled, a long hold and a fresh start. */
void bumped(MindMemory& memory, s32 shortWait, s32 longWait) {
    if (std::abs(memory.route) <= 2) {
        ++memory.collided;
        hold(memory, shortWait);
    } else {
        hold(memory, longWait);
        memory.collided = 0;
        memory.route = 0;
    }
    if (memory.collided >= kCornerTurns) {
        memory.route = -memory.route * 2;
        memory.collided = 0;
    }
}

void countDown(MindMemory& memory, s32 ticks) {
    if (memory.deadEnd > 0) {
        memory.deadEnd -= ticks;
    }
}

/** Reinitialize only the state belonging to the entered strategy. Headings and shared
 * mode/side flags survive: a temporary pursuit is not a newly spawned enemy. */
void resetMind(MindMemory& memory, s32 way, const MindSense& sense) {
    memory.deadEnd = 0;
    switch (way) {
    case kSeekWay:
    case kLurkWay:
        memory.route = 1;
        memory.collided = 0;
        break;
    case kProwlWay:
    case kMirroredProwlWay: memory.counter = 0; break;
    case kWanderWay:
    case kWanderOtherWay: memory.turns = 0; break;
    case kChaseWay:
        memory.route = 0;
        memory.collided = 0;
        memory.stuck = 0;
        memory.skirting = false;
        break;
    case kZigZagWay:
        memory.route = 0;
        memory.collided = 0;
        memory.stuck = 0;
        memory.zigZag.count = 0;
        memory.zigZag.hold = 0;
        break;
    case kSkirmishWay:
    case kSkirmishBombWay:
    case kThrowWay:
    case kBombWay:
        memory.fuse = static_cast<s32>(sense.random % kFirstThrowWaitSpread);
        memory.turns = 0;
        memory.stuck = 0;
        break;
    case kFleeWay: memory.counter = 0; break;
    case kStandCastWay:
    case kRangeCastWay:
        memory.primed = true;
        memory.fuse = static_cast<s32>(sense.random % kFirstWaitSpread);
        memory.counter = 0;
        memory.turns = 0;
        break;
    case kCastWay:
        memory.route = 1;
        memory.collided = 0;
        memory.primed = true;
        memory.fuse = kCastFirstWait + static_cast<s32>(sense.random % kCastFirstWait);
        break;
    case kLungeWay:
        memory.primed = true;
        memory.counter = static_cast<s32>(sense.random % kFirstWaitSpread);
        break;
    default: break;
    }
}

/** The first call adopts birth-initialized memory; later calls format real transitions. */
bool enterMind(MindMemory& memory, s32 way, const MindSense& sense) {
    const bool changed = memory.effectiveWay >= 0 && memory.effectiveWay != way;
    if (changed) {
        resetMind(memory, way, sense);
    }
    memory.effectiveWay = way;
    return changed;
}

/** Alternate the side an unseen enemy takes around walls by its slot. */
MindIntent fallbackWander(MindMemory& memory, const MindSense& sense) {
    return enemyMindOf(sense.mirroredWander ? kWanderOtherWay : kWanderWay).think(memory, sense);
}

/** Straight at the player, or the first clear heading swept along its route's side, out to
 * half a turn, without returning to a just-abandoned heading or reversing in place. */
class SeekMind : public EnemyMind {
public:
    explicit SeekMind(s32 way) : m_way(way) {}
    std::string_view name() const override { return "seek"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (!sense.recognized || sense.target < 0) {
            return fallbackWander(memory, sense);
        }
        enterMind(memory, m_way, sense);
        return steer(memory, sense);
    }

    static MindIntent steer(MindMemory& memory, const MindSense& sense) {
        countDown(memory, sense.ticks);
        MindIntent intent;
        if (memory.deadEnd > 0) {
            intent.heading = memory.heading;
            return intent;
        }
        const f32 face = sense.faceAngle(memory.heading);
        memory.seekHeading = face;
        f32 heading = face;
        for (s32 k = 0; k <= 8; ++k) {
            const f32 offset =
                static_cast<f32>(k) * (kPi / 8.0f) * (memory.route > 0 ? 1.0f : -1.0f);
            const f32 candidate = wrapAngle(face + offset);
            if ((std::abs(memory.heading - memory.headingBefore) > kHeadingSlack &&
                 std::abs(candidate - memory.headingBefore) <= kHeadingSlack) ||
                std::abs(wrapAngle(candidate - memory.heading)) >= kPi - kHeadingSlack) {
                continue;
            }
            if (sense.clearAlong(candidate)) {
                heading = candidate;
                memory.collided = k;
                break;
            }
        }
        memory.headingBefore = memory.heading;
        memory.heading = heading;
        intent.heading = heading;
        return intent;
    }

private:
    s32 m_way;
};

/** Probe ahead and turn a quarter to its own side around obstructions. A twenty-tick hold
 * ends with another quarter turn, but does not prevent an intervening probe from turning. */
class WanderMind : public EnemyMind {
public:
    WanderMind(f32 turn, s32 way) : m_turn(turn), m_way(way) {}
    std::string_view name() const override { return "wander"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        enterMind(memory, m_way, sense);
        if (memory.deadEnd > 0) {
            memory.deadEnd -= sense.ticks;
            if (memory.deadEnd <= 0) {
                memory.heading = wrapAngle(memory.heading + m_turn);
                memory.turns = (memory.turns + 1) % kWanderCycle;
            }
        }
        if (!sense.wanderClearAlong(memory.heading)) {
            memory.heading = wrapAngle(memory.heading + m_turn);
            hold(memory, kWanderWait);
        }
        MindIntent intent;
        intent.heading = memory.heading;
        return intent;
    }

private:
    f32 m_turn;
    s32 m_way;
};

/** The rat's way (move_logic02, 04): after a player only while one is within eight (its
 * crowded distance), else straight on; a stop holds it a while and then turns it an eighth
 * of a turn, to its own side, and the fourth such turn changes it for good to the other
 * side's way. A player against it is faced. */
class ProwlMind : public EnemyMind {
public:
    ProwlMind(f32 turn, s32 mirror) : m_turn(turn), m_mirror(mirror) {}
    std::string_view name() const override { return "prowl"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.target >= 0 && sense.closeDistance <= kProwlPounce) {
            return enemyMindOf(kSeekWay).think(memory, sense);
        }
        const bool changed =
            enterMind(memory, m_mirror == kProwlWay ? kMirroredProwlWay : kProwlWay, sense);
        MindIntent intent;
        if (!changed && sense.blocked) {
            hold(memory, kProwlWait);
        }
        if (memory.deadEnd > 0) {
            memory.deadEnd -= sense.ticks;
            if (memory.deadEnd <= 0) {
                memory.heading = wrapAngle(memory.heading + m_turn);
                if (++memory.counter >= kProwlTurns) {
                    memory.counter = 0;
                    intent.become = m_mirror;
                }
            }
        }
        if (sense.contact >= 0) {
            memory.heading = yawBetween(sense.position, sense.contactPosition);
        }
        intent.heading = memory.heading;
        return intent;
    }

private:
    f32 m_turn;
    s32 m_mirror;
};

/**
 * The corner-hugging chase. Straight at the player while the way is open; something in the
 * way, it goes round on the side that gets nearer, turning a sixteenth of a turn further from
 * straight until a step keeps it clear, and keeps skirting until the straight way is open
 * again (the original tries the same offsets, but only after each dead stop, and slides back
 * under the player between them). A dead stop holds the heading a while and counts a bump;
 * seven bumps and the route doubles back; ten refused headings and it goes straight anyway.
 * With no player seen it wanders.
 */
class ChaseMind : public EnemyMind {
public:
    std::string_view name() const override { return "chase"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (!sense.recognized || sense.target < 0) {
            return fallbackWander(memory, sense);
        }
        const bool changed = enterMind(memory, kChaseWay, sense);
        if (!changed && sense.blocked) {
            if (sense.bumpedOther && (memory.route == 0 || std::abs(memory.route) > 2)) {
                memory.route = sense.otherSide;
                memory.collided = 0;
            }
            if (sense.bumpedOther) {
                bumped(memory, kShortWaitOther, kLongWaitOther);
            } else {
                bumped(memory, kShortWait, kLongWait);
            }
        }
        countDown(memory, sense.ticks);
        const f32 face = sense.faceAngle(memory.heading);
        MindIntent intent;
        if (memory.deadEnd > 0) {
            intent.heading = memory.heading;
            return intent;
        }
        f32 candidate = face;
        bool refused = false;
        if (sense.contact >= 0 || sense.openAlong(face)) {
            memory.skirting = false;
        } else {
            // Round it: further from straight on the route's side until a step is clear,
            // failing that on the other side, failing that straight anyway.
            if (memory.route == 0) {
                memory.route = sense.nearerSide();
            }
            const s32 side = memory.route > 0 ? 1 : -1;
            bool found = false;
            for (const s32 s : {side, -side}) {
                for (usize k = 1; k < kCornerOffsets.size() && !found; ++k) {
                    const f32 tried = wrapAngle(face + static_cast<f32>(s) * kCornerOffsets[k]);
                    if (sense.openAlong(tried)) {
                        candidate = tried;
                        found = true;
                        if (s != side) {
                            memory.route = s;
                        }
                    }
                }
                if (found) {
                    break;
                }
            }
            memory.skirting = found;
            refused = !found;
        }
        const bool turnedBack =
            std::abs(wrapAngle(memory.heading - memory.headingBefore)) > kHeadingSlack &&
            std::abs(wrapAngle(candidate - memory.headingBefore)) <= kHeadingSlack;
        if (turnedBack || refused || !sense.clearAlong(candidate)) {
            refused = true;
            ++memory.stuck;
        } else {
            memory.stuck = 0;
        }
        if (memory.stuck > kStuckTurns) {
            candidate = face;
            memory.heading = face;
        }
        if (!refused) {
            memory.headingBefore = memory.heading;
            memory.heading = candidate;
        }
        intent.heading = candidate;
        intent.turn = !refused || candidate == face;
        return intent;
    }
};

/** Standing where it is, facing its player, and throwing whenever it may: the player in
 * sight and within ten above or below. The animation sequencer owns firing cadence;
 * the separate AI hold is counted down only while the player can be shot. */
class ThrowMind : public EnemyMind {
public:
    explicit ThrowMind(s32 way) : m_way(way) {}
    std::string_view name() const override { return "throw"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        enterMind(memory, m_way, sense);
        MindIntent intent;
        intent.heading = sense.faceAngle(memory.heading);
        memory.heading = intent.heading;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        if (sense.onScreen && sense.target >= 0 && sense.targetDistance <= sense.sight &&
            std::abs(sense.targetVertical) <= kThrowReach) {
            if (memory.fuse <= 0) {
                intent.throwing = true;
            } else {
                memory.fuse -= sense.ticks;
            }
        }
        return intent;
    }

private:
    s32 m_way;
};

/** The archer's (move_logic16): facing its player and, when they are level with it, backing
 * off weapon up once they come within six tenths of its sight until they are beyond eight
 * tenths again, nudged off straight for each stop, and giving up after eight. Its AI hold
 * runs first, but releasing a missile does not set it: firing cadence belongs to the body. */
class SkirmishMind : public EnemyMind {
public:
    explicit SkirmishMind(s32 way) : m_way(way) {}
    std::string_view name() const override { return "skirmish"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        const bool changed = enterMind(memory, m_way, sense);
        const bool blocked = !changed && sense.blocked;
        // A fresh stop starts the nudges over.
        if (blocked && memory.stuck == 0) {
            memory.turns = 0;
        }
        memory.stuck = blocked ? 1 : 0;
        MindIntent intent;
        const f32 face = sense.faceAngle(memory.heading);
        memory.heading = face;
        intent.heading = face;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        if (!sense.onScreen || sense.target < 0 || std::abs(sense.targetVertical) > kThrowReach) {
            return intent;
        }
        f32 nudge = 0.0f;
        if (!memory.keepingOff) {
            memory.keepingOff = sense.targetDistance <= kKeepOffFrom * sense.sight;
        } else if (sense.targetDistance > kKeepOffTo * sense.sight) {
            memory.keepingOff = false;
        } else if (blocked) {
            if (memory.turns < static_cast<s32>(kFleeNudges.size())) {
                nudge = kFleeNudges[static_cast<usize>(memory.turns++)];
            } else {
                memory.keepingOff = false;
            }
        }
        if (memory.fuse > 0) {
            memory.fuse -= sense.ticks;
        } else if (!memory.keepingOff) {
            intent.throwing = true;
        } else {
            // Away from the player, facing them still.
            intent.heading = wrapAngle(face + kPi + nudge);
            intent.turn = false;
            intent.facing = face;
            intent.pace = kKeepOffPace;
            intent.action = EnemyAction::RunAttack;
        }
        return intent;
    }

private:
    s32 m_way;
};

/** The suicide's: still until a player is within sight, a second of fuse, then a run at
 * them half as fast again, blowing up against them or after four seconds anyway. */
class SuicideMind : public EnemyMind {
public:
    std::string_view name() const override { return "suicide"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        enterMind(memory, kSuicideWay, sense);
        MindIntent intent;
        intent.heading = sense.faceAngle(memory.heading);
        memory.heading = intent.heading;
        switch (memory.mode) {
        case 0:
            intent.pace = 0.0f;
            intent.action = EnemyAction::Ready;
            if (sense.target >= 0 && sense.targetDistance <= sense.sight) {
                memory.mode = 1;
                memory.fuse = kFuseTicks;
                memory.counter = 0;
            }
            break;
        case 1:
            intent.pace = 0.0f;
            intent.action = EnemyAction::Ready;
            if (sense.target >= 0 && sense.action != EnemyAction::Run) {
                memory.fuse -= sense.ticks;
                if (memory.fuse <= 0) {
                    intent.action = EnemyAction::ReadyToWalk;
                }
            }
            // move_logic18 8004AC68 waits for RUN, not its wind-up. Movement,
            // the burn clock and the yell begin together on that same update.
            if (sense.action != EnemyAction::Run) {
                break;
            }
            memory.mode = 2;
            intent.yell = true;
            [[fallthrough]];
        default:
            memory.counter += sense.ticks;
            intent.pace = kSuicidePace;
            intent.action = EnemyAction::Run;
            if (memory.counter >= kBurnTicks || sense.contact >= 0) {
                intent.explode = true;
            }
            break;
        }
        return intent;
    }
};

/** Turning on the spot where it was born, and done with once its generator is gone. */
class LoiterMind : public EnemyMind {
public:
    std::string_view name() const override { return "loiter"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        enterMind(memory, kLoiterWay, sense);
        memory.heading = wrapAngle(memory.heading + kLoiterTurn * static_cast<f32>(sense.ticks));
        MindIntent intent;
        intent.heading = memory.heading;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        intent.expire = sense.generatorGone;
        return intent;
    }
};

/** Away from the lit suicide bomber near it at twice its pace, nudged further off straight
 * for each step that gets nowhere (move_logic24); with none, it wanders. */
class FleeMind : public EnemyMind {
public:
    std::string_view name() const override { return "flee"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (!sense.bomber.has_value()) {
            return fallbackWander(memory, sense);
        }
        const bool changed = enterMind(memory, kFleeWay, sense);
        f32 nudge = 0.0f;
        if (changed || !sense.blocked) {
            memory.counter = 0;
        } else if (memory.counter < static_cast<s32>(kFleeNudges.size())) {
            nudge = kFleeNudges[static_cast<usize>(memory.counter++)];
        }
        memory.heading = wrapAngle(yawBetween(sense.position, *sense.bomber) + kPi + nudge);
        MindIntent intent;
        intent.heading = memory.heading;
        intent.pace = kFleePace;
        intent.action = EnemyAction::Run;
        return intent;
    }
};

/** Still until a player comes within sight, then after them with sight without end. */
class LurkMind : public EnemyMind {
public:
    std::string_view name() const override { return "lurk"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (!memory.woken && sense.target >= 0 && sense.targetDistance <= sense.sight) {
            memory.woken = true;
        }
        if (memory.woken) {
            MindIntent intent = enemyMindOf(kSeekWay).think(memory, sense);
            intent.become = kSeekWay;
            return intent;
        }
        enterMind(memory, kLurkWay, sense);
        MindIntent intent;
        intent.heading = memory.heading;
        intent.pace = 0.0f;
        intent.turn = false;
        intent.action = EnemyAction::Ready;
        return intent;
    }
};

/** Standing where it is, facing whoever comes against it. */
/** The ranged casters' shared business: their first wait drawn at random, then an attack asked
 * for whenever the wait is out and the player is seen within sight and ten above or below (the
 * power attack from the third strength, else the two attacks in turn). True when it asked. */
/** A ranged caster's first wait, drawn once as it takes up its way (format_brain). */
void primeRangedWait(MindMemory& memory, const MindSense& sense) {
    if (!memory.primed) {
        memory.primed = true;
        memory.fuse = static_cast<s32>(sense.random % kFirstWaitSpread);
    }
}

bool askRangedAttack(MindMemory& memory, const MindSense& sense, MindIntent& intent) {
    primeRangedWait(memory, sense);
    if (!sense.onScreen || sense.target < 0 || !sense.recognized ||
        sense.targetDistance > sense.sight || std::abs(sense.targetVertical) > kCastReach) {
        return false;
    }
    if (memory.fuse > 0) {
        memory.fuse -= sense.ticks;
        return false;
    }
    constexpr s32 kPowerStrength = 3;
    if (sense.tier >= kPowerStrength) {
        intent.action = EnemyAction::PowerAttack;
    } else {
        intent.action = (++memory.counter & 1) != 0 ? EnemyAction::Attack : EnemyAction::Attack2;
    }
    return true;
}

/** Standing, facing its player and attacking whenever its wait allows, its swing casting at
 * range; within six it chases hand to hand (move_logic28). */
class StandCastMind : public EnemyMind {
public:
    std::string_view name() const override { return "stand-cast"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.target >= 0 && sense.targetDistance <= kHandToHand) {
            return enemyMindOf(kChaseWay).think(memory, sense);
        }
        enterMind(memory, kStandCastWay, sense);
        memory.heading = sense.faceAngle(memory.heading);
        MindIntent intent;
        intent.heading = memory.heading;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        askRangedAttack(memory, sense, intent);
        return intent;
    }
};

/** Keeping between eight and eighteen of its player: backing off inside eight until beyond
 * ten, closing in beyond eighteen until within sixteen, at four fifths of its pace, nudged off
 * straight a step at a time when that gets nowhere; in between it stands and attacks as its
 * wait allows, and within six it chases hand to hand (move_logic29). */
class RangeCastMind : public EnemyMind {
public:
    std::string_view name() const override { return "range-cast"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.target >= 0 && sense.targetDistance <= kHandToHand) {
            return enemyMindOf(kChaseWay).think(memory, sense);
        }
        const bool changed = enterMind(memory, kRangeCastWay, sense);
        const bool blocked = !changed && sense.blocked;
        primeRangedWait(memory, sense);
        memory.heading = sense.faceAngle(memory.heading);
        MindIntent intent;
        intent.heading = memory.heading;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        const bool inReach = sense.onScreen && sense.target >= 0 && sense.recognized &&
                             std::abs(sense.targetVertical) <= kCastReach;
        f32 nudge = 0.0f;
        if (inReach) {
            if (memory.mode == 0) {
                if (sense.targetDistance <= kBackOffWithin) {
                    memory.mode = 1;
                }
                if (sense.targetDistance > kCloseInBeyond) {
                    memory.mode = 2;
                }
            } else {
                if (memory.mode == 1 ? sense.targetDistance > kBackedOff
                                     : sense.targetDistance <= kClosedIn) {
                    memory.mode = 0;
                }
                if (memory.mode != 0 && blocked) {
                    if (memory.turns < static_cast<s32>(kFleeNudges.size())) {
                        nudge = kFleeNudges[static_cast<usize>(memory.turns++)];
                    } else {
                        memory.mode = 0;
                    }
                }
            }
            if (!blocked) {
                memory.turns = 0;
            }
        }
        // Stopped by the world, it attacks where it stands instead.
        if (memory.mode != 0 && memory.fuse <= 0 && inReach && !sense.bumpedWall) {
            const f32 away = memory.mode == 1 ? kPi : 0.0f;
            intent.heading = wrapAngle(memory.heading + away + nudge);
            intent.pace = kRangePace;
            intent.action = EnemyAction::Walk;
            intent.turn = memory.mode != 1; // backing off, it keeps facing them
            intent.facing = memory.heading;
            return intent;
        }
        askRangedAttack(memory, sense, intent);
        return intent;
    }
};

/** Zig-zagging at its player: a quarter turn every 45 ticks, always to the same side as it
 * starts, and after four swings, once it has drifted more than a quarter turn off its player
 * (or at once when a step gets nowhere), a fresh heading an eighth of a turn and more off
 * straight at them, held thirty ticks. Unseen it wanders; within eight it seeks
 * (move_logic14). */
class ZigZagMind : public EnemyMind {
public:
    std::string_view name() const override { return "zig-zag"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.target < 0 || !sense.recognized) {
            return fallbackWander(memory, sense);
        }
        if (sense.closeDistance <= kZigZagCloseIn) {
            return enemyMindOf(kSeekWay).think(memory, sense);
        }
        enterMind(memory, kZigZagWay, sense);
        ZigZag& zig = memory.zigZag;
        zig.count -= sense.ticks;
        if (zig.count <= 0) {
            zig.side = -zig.side;
            memory.heading = wrapAngle(memory.heading + (zig.side > 0 ? kPi / 2.0f : -kPi / 2.0f));
            zig.count += kSwingTicks;
            ++zig.swings;
        }
        MindIntent intent;
        intent.heading = memory.heading;
        return intent;
    }

    void afterMove(MindMemory& memory, const MindSense& before,
                   const MindSense& after) const override {
        if (memory.effectiveWay != kZigZagWay) {
            return;
        }
        ZigZag& zig = memory.zigZag;
        const f32 face = before.faceAngle(memory.heading);
        const f32 drift = wrapAngle(face - memory.heading);
        if (zig.hold <= 0 && (after.blocked || memory.deadEnd > 0 ||
                              (zig.swings >= kSwingsBeforeAim && std::abs(drift) > kPi / 2.0f))) {
            const f32 offset = kAimOffset + (kAimOffsetGrowth * static_cast<f32>(zig.spread));
            if (zig.swings >= kSwingsBeforeAim) {
                zig.side = -zig.side;
            }
            const f32 playerFacing = yawBetween(
                after.position, after.targetPlayerPosition.value_or(after.targetPosition));
            memory.heading = wrapAngle(playerFacing + (zig.side > 0 ? offset : -offset));
            memory.deadEnd = 0;
            zig.count = 0;
            zig.swings = 0;
            ++zig.spread;
            zig.hold = kAimHold;
        } else {
            if (zig.spread > 0) {
                --zig.spread;
            }
            zig.hold -= before.ticks;
        }
    }
};

/** Walking the level's lookouts: to the nearest, then on to the one each names, until a player
 * comes within four fifths of its sight, when it seeks them, taking up the round again from
 * the nearest lookout once they are further off (move_logic15). With no lookouts it wanders. */
class PatrolMind : public EnemyMind {
public:
    std::string_view name() const override { return "patrol"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.target >= 0 && sense.closeDistance <= kPatrolNotice * sense.sight &&
            flatDistance(sense.targetPosition, sense.position) <= kPatrolNotice * sense.sight) {
            memory.lookout = -1;
            return enemyMindOf(kSeekWay).think(memory, sense);
        }
        if (sense.lookouts == nullptr || sense.lookouts->empty()) {
            return fallbackWander(memory, sense);
        }
        const LookoutRoute& route = *sense.lookouts;
        if (memory.lookout < 0 || static_cast<usize>(memory.lookout) >= route.points.size()) {
            f32 best = 0.0f;
            memory.lookout = -1;
            for (usize i = 0; i < route.points.size(); ++i) {
                if (i >= route.next.size() || route.next[i] < 0) {
                    continue;
                }
                const f32 distance = glm::distance(route.points[i], sense.position);
                if (memory.lookout < 0 || distance < best) {
                    best = distance;
                    memory.lookout = static_cast<s32>(i);
                }
            }
        }
        if (memory.lookout < 0) {
            return fallbackWander(memory, sense);
        }
        enterMind(memory, kPatrolWay, sense);
        const Vec3& point = route.points[static_cast<usize>(memory.lookout)];
        memory.heading = yawBetween(sense.position, point);
        if (std::abs(point.y - sense.position.y) < kLookoutHeight &&
            flatDistance(point, sense.position) < kLookoutReached) {
            memory.lookout = route.next[static_cast<usize>(memory.lookout)];
        }
        MindIntent intent;
        intent.heading = memory.heading;
        return intent;
    }
};

/** A caster wanders unseen and chases up close; otherwise it seeks and requests a cast when
 * its timer expires. The animation owns the swing; the temporary steering delegate clears
 * the movement hold while preserving the next cast's randomized cooldown. */
class CastMind : public EnemyMind {
public:
    std::string_view name() const override { return "cast"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.target < 0 || !sense.recognized) {
            return fallbackWander(memory, sense);
        }
        if (sense.targetDistance <= kCastCloseIn) {
            return enemyMindOf(kChaseWay).think(memory, sense);
        }
        const s32 previousWay = memory.effectiveWay < 0 ? kCastWay : memory.effectiveWay;
        enterMind(memory, kCastWay, sense);
        if (!memory.primed) {
            memory.primed = true;
            memory.fuse = kCastFirstWait + static_cast<s32>(sense.random % kCastFirstWait);
        }
        if (memory.deadEnd <= 0) {
            memory.fuse -= sense.ticks;
        }
        if (memory.deadEnd <= 0 && memory.fuse <= 0) {
            const s32 window = kCastLeast + static_cast<s32>(sense.random % kCastSpread);
            memory.deadEnd = window;
            const s32 spread = std::max(sense.castWait / 2, 1);
            memory.fuse = window + static_cast<s32>((sense.random >> 8U) % spread) + sense.castWait;
        }
        const bool casting = memory.deadEnd > 0 && sense.onScreen;
        // The steering delegate temporarily runs as Seek, but this remains a Cast tick.
        // Its entry reset leaves the cast cooldown intact and clears the movement hold.
        if (previousWay != kSeekWay) {
            resetMind(memory, kSeekWay, sense);
        }
        MindIntent intent = SeekMind::steer(memory, sense);
        if (casting) {
            intent.action = sense.tier >= 2 ? EnemyAction::PowerAttack : EnemyAction::Attack;
        }
        return intent;
    }
};

/** Facing its player, it creeps up at half pace and every half second to a second
 * attacks: a lunge at full pace within ten, landing within seven and a half, else its power
 * attack (move_logic31). With nobody to face, it stands. */
class LungeMind : public EnemyMind {
public:
    std::string_view name() const override { return "lunge"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        enterMind(memory, kLungeWay, sense);
        if (!memory.primed) {
            memory.primed = true;
            memory.counter = static_cast<s32>(sense.random % kFirstWaitSpread);
        }
        MindIntent intent;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        memory.heading = sense.faceAngle(memory.heading);
        intent.heading = memory.heading;
        if (sense.target < 0 || !sense.recognized || !sense.onScreen) {
            return intent;
        }
        const s32 wait = kLungeWait + static_cast<s32>(sense.random % kLungeWait);
        switch (sense.action) {
        case EnemyAction::Attack:
        case EnemyAction::AttackRecover:
            intent.pace = 1.0f;
            memory.counter = wait;
            intent.strike = sense.targetDistance <= kLungeLands;
            break;
        case EnemyAction::PowerAttack:
        case EnemyAction::PowerAttackRecover: memory.counter = wait; break;
        default:
            if (memory.counter <= 0) {
                intent.action = sense.targetDistance <= kLungeFrom ? EnemyAction::Attack
                                                                   : EnemyAction::PowerAttack;
            } else if (sense.action != EnemyAction::Start) {
                memory.counter -= sense.ticks;
                intent.pace = kCreepPace;
                intent.action = EnemyAction::Walk;
            }
            break;
        }
        return intent;
    }
};

const SeekMind kSeek{kSeekWay};
const SeekMind kSeekAlias{kSeekAliasWay};
const WanderMind kWander{-kWanderTurn, kWanderWay};
const WanderMind kMirroredWander{kWanderTurn, kWanderOtherWay};
const ProwlMind kProwl{kProwlTurn, kMirroredProwlWay};
const ProwlMind kMirroredProwl{-kProwlTurn, kProwlWay};
const ChaseMind kChase;
const LoiterMind kLoiter;
const FleeMind kFlee;
const LurkMind kLurk;
const LungeMind kLunge;
const CastMind kCast;
const PatrolMind kPatrol;
const ZigZagMind kZigZag;
const StandCastMind kStandCast;
const RangeCastMind kRangeCast;
const ThrowMind kThrow{kThrowWay};
const ThrowMind kBomb{kBombWay};
const SkirmishMind kSkirmish{kSkirmishWay};
const SkirmishMind kSkirmishBomb{kSkirmishBombWay};
const SuicideMind kSuicide;

} // namespace

void initializeEnemyMind(MindMemory& memory, s32 algorithm, u32 random) {
    MindSense sense;
    sense.random = random;
    resetMind(memory, algorithm, sense);
    memory.effectiveWay = algorithm;
}

SeekResponse finishSeekStep(MindMemory& memory, SeekContact contact, s32 side) {
    // blocked00 in do_enemy_collide (80045488), followed by do_enemy_move
    // (80044664): only a physical stop counts, not a refused heading probe.
    SeekResponse response;
    s32 wait = 0;
    const auto doubled = [](s32 route) {
        // The target's 32-bit absolute value of INT_MIN remains negative.
        return route != std::numeric_limits<s32>::min() && std::abs(route) > 2;
    };
    switch (contact) {
    case SeekContact::Clear:
        if (memory.deadEnd <= 0) {
            memory.route = 1;
            memory.collided = 0;
        }
        return response;
    case SeekContact::World:
        wait = doubled(memory.route) ? kLongWait : kSeekWait;
        ++memory.collided;
        if (memory.collided >= kSeekReverseStops) {
            // Repeated dead ends can exhaust all 32 bits. Preserve the native
            // wrap without signed multiplication overflow in the host language.
            memory.route = std::bit_cast<s32>(0U - 2U * std::bit_cast<u32>(memory.route));
            memory.collided = 0;
            if (doubled(memory.route)) {
                memory.heading = memory.seekHeading;
                response.faceTarget = true;
            }
        }
        break;
    case SeekContact::Enemy:
    case SeekContact::Item:
        memory.route = side;
        wait = kLongWait;
        break;
    case SeekContact::Critter: wait = kWanderWait; break;
    }
    if (memory.deadEnd <= 0) {
        memory.deadEnd = wait;
        // fn_8004D030 never extends a running hold or cancels attacks/reactions.
        response.stopWalking = wait >= kLongWait;
    }
    return response;
}

f32 wrapAngle(f32 angle) {
    while (angle > kPi) {
        angle -= 2.0f * kPi;
    }
    while (angle <= -kPi) {
        angle += 2.0f * kPi;
    }
    return angle;
}

f32 MindSense::faceAngle(f32 fallback) const {
    return target >= 0 ? yawBetween(position, targetPosition) : fallback;
}

s32 MindSense::nearerSide() const {
    if (target < 0) {
        return 1;
    }
    const f32 dx = position.x - targetPosition.x;
    const f32 dz = position.z - targetPosition.z;
    const f32 left = wrapAngle(yaw + kPi / 6.0f);
    const f32 right = wrapAngle(yaw - kPi / 6.0f);
    const f32 x1 = dx + std::sin(left);
    const f32 z1 = dz + std::cos(left);
    const f32 x2 = dx + std::sin(right);
    const f32 z2 = dz + std::cos(right);
    return x2 * x2 + z2 * z2 <= x1 * x1 + z1 * z1 ? -1 : 1;
}

LookoutRoute LookoutRoute::of(std::span<const WorldLocator> locators) {
    LookoutRoute route;
    for (const WorldLocator& locator : locators) {
        if ((locator.kind != LocatorKind::Sentry && locator.kind != LocatorKind::Event) ||
            route.points.size() >= kMost) {
            continue;
        }
        route.points.push_back(locator.position);
        route.next.push_back(static_cast<s32>(locator.next));
    }
    return route;
}

bool fleesBombers(s32 algorithm) {
    switch (algorithm) {
    case kSeekWay:
    case 1:
    case kProwlWay:
    case kMirroredProwlWay:
    case kWanderWay:
    case kWanderOtherWay:
    case kChaseWay:
    case 8:
    case 9:
    case 10:
    case 12:
    case 13:
    case 14:
    case 15:
    case kSkirmishWay:
    case kSkirmishBombWay:
    case 22:
    case 29:
    case 30: return true;
    default: return false;
    }
}

const EnemyMind& enemyMindOf(s32 algorithm) {
    switch (algorithm) {
    case kSeekWay: return kSeek;
    case kSeekAliasWay: return kSeekAlias;
    case kProwlWay: return kProwl;
    case kMirroredProwlWay: return kMirroredProwl;
    case kChaseWay: return kChase;
    case kLoiterWay: return kLoiter;
    case kFleeWay: return kFlee;
    case kLurkWay: return kLurk;
    case kLungeWay: return kLunge;
    case kCastWay: return kCast;
    case kPatrolWay: return kPatrol;
    case kZigZagWay: return kZigZag;
    case kStandCastWay: return kStandCast;
    case kRangeCastWay: return kRangeCast;
    case kThrowWay: return kThrow;
    case kBombWay: return kBomb;
    case kSkirmishWay: return kSkirmish;
    case kSkirmishBombWay: return kSkirmishBomb;
    case kSuicideWay: return kSuicide;
    case kWanderOtherWay: return kMirroredWander;
    case kWanderWay:
    default: return kWander;
    }
}

} // namespace gdl::game
