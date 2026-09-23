#include "game/enemies/EnemyMind.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace gdl::game {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kHeadingSlack = 0.0349f; ///< two degrees: a heading this close is the same
constexpr int kStuckTurns = 10;          ///< probes refused before going straight anyway
constexpr int kCornerTurns = 7;          ///< bumps before the route doubles the other way
constexpr int kShortWait = 10;           ///< ticks a heading is held after a bump
constexpr int kLongWait = 60;
constexpr int kShortWaitOther = 15; ///< when the bump was another enemy
constexpr int kLongWaitOther = 50;
constexpr float kWanderTurn = kPi / 4.0f; ///< a wanderer turns this much at a dead end
constexpr int kWanderWait = 30;
constexpr float kProwlPounce = 8.0f; ///< a prowler goes for a player this close
constexpr float kLoiterTurn = kPi / 64.0f;
constexpr float kThrowReach = 10.0f; ///< a thrower's player must be within this above or below
constexpr float kKeepOffFrom = 0.6f; ///< of its sight, a skirmisher backs off from
constexpr float kKeepOffTo = 0.8f;   ///< of its sight, and stops at
constexpr float kKeepOffPace = 0.8f;
constexpr int kFuseTicks = 60;
constexpr int kBurnTicks = 240; ///< a suicide's run before it blows up anyway
constexpr float kSuicidePace = 1.5f;

// The corner-hugging offsets, one more sixteenth of a turn for every bump.
constexpr std::array<float, 8> kCornerOffsets{
    0.0f,       kPi / 8.0f,        kPi / 4.0f,        3.0f * kPi / 8.0f,
    kPi / 2.0f, 5.0f * kPi / 8.0f, 3.0f * kPi / 4.0f, 7.0f * kPi / 8.0f};

float yawBetween(const Vec3& from, const Vec3& to) {
    return std::atan2(to.x - from.x, to.z - from.z);
}

/** A hold on the heading, unless one is already running. */
void hold(MindMemory& memory, int ticks) {
    if (memory.deadEnd <= 0) {
        memory.deadEnd = ticks;
    }
}

/** The bookkeeping a bump does for the corner-huggers: a short hold and one more bump on
 * the route, or, on a route already doubled, a long hold and a fresh start. */
void bumped(MindMemory& memory, int shortWait, int longWait) {
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

void countDown(MindMemory& memory, int ticks) {
    if (memory.deadEnd > 0) {
        memory.deadEnd -= ticks;
    }
}

/** Straight at the player, or the nearest clear heading either side of straight, out to
 * half a turn. */
class SeekMind : public EnemyMind {
public:
    std::string_view name() const override { return "seek"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        countDown(memory, sense.ticks);
        MindIntent intent;
        if (memory.deadEnd > 0) {
            intent.heading = memory.heading;
            return intent;
        }
        const float face = sense.faceAngle(memory.heading);
        float heading = face;
        for (int k = 0; k <= 8; ++k) {
            const int turns = (k + 1) / 2; // out from straight, a sixteenth either side
            const float offset =
                static_cast<float>(turns) * (kPi / 8.0f) * (k % 2 == 0 ? 1.0f : -1.0f);
            const float candidate = wrapAngle(face + offset);
            if (sense.clearAlong(candidate)) {
                heading = candidate;
                break;
            }
        }
        memory.heading = heading;
        intent.heading = heading;
        return intent;
    }
};

/** Straight on until something is in the way, then an eighth of a turn round; a player
 * against it is faced. */
class WanderMind : public EnemyMind {
public:
    std::string_view name() const override { return "wander"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        countDown(memory, sense.ticks);
        if (sense.contact >= 0) {
            memory.heading = yawBetween(sense.position, sense.contactPosition);
        } else if ((sense.bumpedWall || sense.bumpedOther) && memory.deadEnd <= 0) {
            memory.heading =
                wrapAngle(memory.heading + (memory.route < 0 ? -kWanderTurn : kWanderTurn));
            hold(memory, kWanderWait);
            ++memory.turns;
        }
        MindIntent intent;
        intent.heading = memory.heading;
        return intent;
    }
};

/** The rat's way: about its business until a player comes within eight, then after it for
 * good. */
class ProwlMind : public EnemyMind {
public:
    std::string_view name() const override { return "prowl"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.target >= 0 && sense.targetDistance <= kProwlPounce) {
            MindIntent intent = enemyMindOf(kSeekWay).think(memory, sense);
            intent.become = kSeekWay;
            return intent;
        }
        return enemyMindOf(kWanderWay).think(memory, sense);
    }
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
            return enemyMindOf(kWanderWay).think(memory, sense);
        }
        if (sense.blocked) {
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
        const float face = sense.faceAngle(memory.heading);
        MindIntent intent;
        if (memory.deadEnd > 0) {
            intent.heading = memory.heading;
            return intent;
        }
        float candidate = face;
        bool refused = false;
        if (sense.contact >= 0 || sense.openAlong(face)) {
            memory.skirting = false;
        } else {
            // Round it: further from straight on the route's side until a step is clear,
            // failing that on the other side, failing that straight anyway.
            if (memory.route == 0) {
                memory.route = sense.nearerSide();
            }
            const int side = memory.route > 0 ? 1 : -1;
            bool found = false;
            for (const int s : {side, -side}) {
                for (std::size_t k = 1; k < kCornerOffsets.size() && !found; ++k) {
                    const float tried = wrapAngle(face + static_cast<float>(s) * kCornerOffsets[k]);
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
 * sight and within ten above or below, the wait since the last throw over. */
class ThrowMind : public EnemyMind {
public:
    std::string_view name() const override { return "throw"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.threw) {
            memory.fuse = sense.idleTicks;
        } else if (memory.fuse > 0) {
            memory.fuse -= sense.ticks;
        }
        MindIntent intent;
        intent.heading = sense.faceAngle(memory.heading);
        memory.heading = intent.heading;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        if (sense.target >= 0 && sense.targetDistance <= sense.sight &&
            std::abs(sense.targetVertical) <= kThrowReach && memory.fuse <= 0) {
            intent.throwing = true;
        }
        return intent;
    }
};

/** The archer's: throwing from range, and backing off, weapon up, when the player comes
 * within six tenths of its sight, until they are beyond eight tenths again. */
class SkirmishMind : public EnemyMind {
public:
    std::string_view name() const override { return "skirmish"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.threw) {
            memory.fuse = sense.idleTicks;
        } else if (memory.fuse > 0) {
            memory.fuse -= sense.ticks;
        }
        MindIntent intent;
        const float face = sense.faceAngle(memory.heading);
        memory.heading = face;
        intent.heading = face;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        const bool level = sense.target >= 0 && std::abs(sense.targetVertical) <= kThrowReach;
        if (level) {
            if (!memory.keepingOff && sense.targetDistance <= kKeepOffFrom * sense.sight) {
                memory.keepingOff = true;
            } else if (memory.keepingOff && sense.targetDistance > kKeepOffTo * sense.sight) {
                memory.keepingOff = false;
            }
        }
        if (!level) {
            return intent;
        }
        if (memory.keepingOff) {
            // Away from the player, facing them still.
            intent.heading = wrapAngle(face + kPi);
            intent.turn = false;
            intent.pace = kKeepOffPace;
            intent.action = EnemyAction::RunAttack;
        } else if (memory.fuse <= 0 && sense.targetDistance <= sense.sight) {
            intent.throwing = true;
        }
        return intent;
    }
};

/** The suicide's: still until a player is within sight, a second of fuse, then a run at
 * them half as fast again, blowing up against them or after four seconds anyway. */
class SuicideMind : public EnemyMind {
public:
    std::string_view name() const override { return "suicide"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
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
            memory.fuse -= sense.ticks;
            if (sense.target >= 0 && memory.fuse <= 0) {
                intent.action = EnemyAction::ReadyToWalk; // the fuse lit
                if (sense.action == EnemyAction::Run || sense.action == EnemyAction::ReadyToWalk) {
                    memory.mode = 2;
                }
            }
            break;
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
        memory.heading = wrapAngle(memory.heading + kLoiterTurn * static_cast<float>(sense.ticks));
        MindIntent intent;
        intent.heading = memory.heading;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        intent.expire = sense.generatorGone;
        return intent;
    }
};

/** Away from the player it sees, at a run; nobody seen, it wanders. */
class FleeMind : public EnemyMind {
public:
    std::string_view name() const override { return "flee"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.target < 0) {
            return enemyMindOf(kWanderWay).think(memory, sense);
        }
        countDown(memory, sense.ticks);
        float away = wrapAngle(sense.faceAngle(memory.heading) + kPi);
        if (!sense.clearAlong(away)) {
            for (int k = 1; k <= 4 && !sense.clearAlong(away); ++k) {
                away = wrapAngle(away + static_cast<float>(k) * (kPi / 4.0f) *
                                            (k % 2 == 0 ? 1.0f : -1.0f));
            }
        }
        memory.heading = away;
        MindIntent intent;
        intent.heading = away;
        intent.pace = 1.5f;
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
        MindIntent intent;
        intent.heading = memory.heading;
        intent.pace = 0.0f;
        intent.turn = false;
        intent.action = EnemyAction::Ready;
        return intent;
    }
};

/** Standing where it is, facing whoever comes against it. */
class StandMind : public EnemyMind {
public:
    std::string_view name() const override { return "stand"; }
    MindIntent think(MindMemory& memory, const MindSense& sense) const override {
        if (sense.contact >= 0) {
            memory.heading = yawBetween(sense.position, sense.contactPosition);
        }
        MindIntent intent;
        intent.heading = memory.heading;
        intent.pace = 0.0f;
        intent.action = EnemyAction::Ready;
        return intent;
    }
};

const SeekMind kSeek;
const WanderMind kWander;
const ProwlMind kProwl;
const ChaseMind kChase;
const LoiterMind kLoiter;
const FleeMind kFlee;
const LurkMind kLurk;
const StandMind kStand;
const ThrowMind kThrow;
const SkirmishMind kSkirmish;
const SuicideMind kSuicide;

} // namespace

float wrapAngle(float angle) {
    while (angle > kPi) {
        angle -= 2.0f * kPi;
    }
    while (angle <= -kPi) {
        angle += 2.0f * kPi;
    }
    return angle;
}

float MindSense::faceAngle(float fallback) const {
    return target >= 0 ? yawBetween(position, targetPosition) : fallback;
}

int MindSense::nearerSide() const {
    if (target < 0) {
        return 1;
    }
    const float dx = position.x - targetPosition.x;
    const float dz = position.z - targetPosition.z;
    const float left = wrapAngle(yaw + kPi / 6.0f);
    const float right = wrapAngle(yaw - kPi / 6.0f);
    const float x1 = dx + std::sin(left);
    const float z1 = dz + std::cos(left);
    const float x2 = dx + std::sin(right);
    const float z2 = dz + std::cos(right);
    return x2 * x2 + z2 * z2 <= x1 * x1 + z1 * z1 ? -1 : 1;
}

const EnemyMind& enemyMindOf(int algorithm) {
    switch (algorithm) {
    case kSeekWay: return kSeek;
    case kProwlWay: return kProwl;
    case kChaseWay: return kChase;
    case kLoiterWay: return kLoiter;
    case kFleeWay: return kFlee;
    case kLurkWay: return kLurk;
    case kStandWay: return kStand;
    case kThrowWay:
    case kBombWay: return kThrow;
    case kSkirmishWay: return kSkirmish;
    case kSuicideWay: return kSuicide;
    case kWanderWay:
    case kWanderOtherWay:
    default: return kWander;
    }
}

} // namespace gdl::game
