#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"
#include "game/enemies/EnemyMind.h"
namespace gdl::game {
namespace {
constexpr f32 kPi = std::numbers::pi_v<f32>;
f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
f32 yawBetween(const Vec3& from, const Vec3& to) {
    return std::atan2(to.x - from.x, to.z - from.z);
}
} // namespace

namespace {
constexpr f32 kMostPush = 40.0f;
// What a hit's flags do to a great one (CritterGetDoAction, CritterDoKnockback).
constexpr u32 kKnockOver = 0x100;  ///< knocked down: KD
constexpr u32 kKnockedBack = 0x20; ///< knocked back: KB
constexpr u32 kShaken = 0x10;      ///< flinches: HITREACT
constexpr u32 kHardPush = 0x10140; ///< the heaviest shoves
constexpr f32 kDeathPush = 20.0f;
constexpr f32 kHardPushScale = 10.0f;
constexpr f32 kKnockBackPush = 7.5f;
constexpr f32 kShakePush = 5.0f;
} // namespace
const EnemyView* Combatant::viewOf(std::span<const EnemyView> players, s32 player) {
    for (const EnemyView& view : players) {
        if (view.player == player) {
            return &view;
        }
    }
    return nullptr;
}

bool Combatant::startMove(Actor& critter, usize index, bool recordUse) {
    const CritterData& data = *critter.definition;
    if (index >= data.moves().size()) {
        return false;
    }
    const MoveDefinition& move = data.moves()[index];
    constexpr u32 kRequiresNode = 0x10;
    if ((move.flags & kRequiresNode) != 0 && !nodeAvailable(critter, move.colnode)) {
        return false;
    }
    const auto sequence = critter.stock->tree->findSequence(move.anim);
    if (!sequence.has_value()) {
        return false;
    }
    if (critter.moveEffect) {
        CombatCue stop;
        stop.critter = m_id;
        stop.stopMoveEffect = true;
        m_cues.push_back(stop);
        critter.moveEffect = false;
    }
    critter.move = static_cast<s32>(index);
    critter.moveDone = false;
    critter.finishedSeconds = 0.0f;
    critter.struckThisMove.clear();
    critter.soundsGiven = 0;
    critter.shotFrame = -1;
    critter.attackTarget.reset();
    critter.stepTarget.reset();
    critter.player.start(critter.stock->tree->sequences[*sequence], *sequence);
    if (recordUse) {
        critter.moveTimes[index] =
            critter.age + static_cast<f32>(std::max(critter.player.frameCount() - 2, 0)) / 30.0f;
    }
    critter.pose.evaluate(*critter.stock->tree, *sequence, 0.0f);
    return true;
}

void Combatant::chooseTarget(Actor& critter, std::span<const EnemyView> players) {
    critter.target = -1;
    critter.targetDistance = 100000.0f;
    const TargetCriteria& sight = critter.definition->sight();
    for (const EnemyView& view : players) {
        if (view.hidden) {
            continue;
        }
        const f32 distance = flatDistance(view.position, critter.position);
        if (!sight.allows(distance, 0.0f, view.position.y - critter.position.y)) {
            continue;
        }
        // On its round of the lookouts it takes only a player within its placement's sight
        // (CritterGetSingleTargetPlayer's visrad).
        if (critter.patrol.active() && critter.patrol.sight() > 0.0f &&
            targetScore(critter, view.position) > critter.patrol.sight()) {
            continue;
        }
        if (distance < critter.targetDistance) {
            critter.targetDistance = distance;
            critter.target = view.player;
        }
    }
    // A player found ends the round for good.
    if (critter.target >= 0 && critter.patrol.active()) {
        critter.patrol.end();
        critter.patrolAim.reset();
    }
}

f32 Combatant::targetScore(const Actor& critter, const Vec3& position) {
    constexpr f32 kSquarelyAhead = 0.5f;
    const f32 distance = flatDistance(position, critter.position);
    const f32 dot = std::cos(wrapAngle(yawBetween(critter.position, position) - critter.yaw));
    return dot > kSquarelyAhead ? distance / dot : 2.0f * distance;
}

std::optional<usize> Combatant::patrolStep(const Actor& critter) {
    if (!critter.patrolAim.has_value()) {
        return std::nullopt;
    }
    constexpr u32 kLinkedOnly = 4;
    const std::span<const MoveDefinition> moves = critter.definition->moves();
    for (usize i = 0; i < moves.size(); ++i) {
        const MoveDefinition& move = moves[i];
        // The steps of the walk family; the one to a point wants a player.
        const bool step =
            move.type >= MoveDefinition::kStepFrom && move.type <= MoveDefinition::kStepLast;
        if (step && move.type != MoveDefinition::kStepToPoint && (move.flags & kLinkedOnly) == 0 &&
            move.speed > 0.0f) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<usize> Combatant::bestMove(const Actor& critter, std::span<const EnemyView> players) {
    const CritterData& data = *critter.definition;
    const EnemyView* view = viewOf(players, critter.target);
    f32 distance = 100000.0f;
    f32 bearing = kPi;
    f32 vertical = 0.0f;
    if (view != nullptr) {
        distance = critter.targetDistance;
        bearing = wrapAngle(yawBetween(critter.position, view->position) - critter.yaw);
        vertical = view->position.y - critter.position.y;
    }
    // With nobody in its sights, one on its round walks to the next lookout.
    if (view == nullptr) {
        if (const auto step = patrolStep(critter); step.has_value()) {
            return step;
        }
    }
    std::optional<usize> best;
    s32 bestPriority = -1;
    const bool patterns =
        critter.stock->definition.selection == CombatantDefinition::Selection::Patterns;
    for (usize i = 0; i < data.moves().size(); ++i) {
        const MoveDefinition& move = data.moves()[i];
        // Attacks, the steps (walks, turns and back-steps, types 48 to 63), the stance and
        // the taunt.
        const bool step =
            move.type >= MoveDefinition::kStepFrom && move.type < MoveDefinition::kStepTo;
        const bool considered =
            step || (!patterns && (move.attack() || move.type == MoveDefinition::kReady ||
                                   move.type == MoveDefinition::kTaunt));
        constexpr u32 kLinkedOnly = 4;
        if (!considered || critter.cooldowns[i] > 0.0f || (move.flags & kLinkedOnly) != 0) {
            continue;
        }
        // Attacks and walks want a player; the stance and the taunt want none in particular.
        if ((move.attack() || step) && view == nullptr) {
            continue;
        }
        if (!move.target.allows(distance, bearing, vertical) ||
            !move.target.allowsPhase(attackRate(critter),
                                     flatDistance(critter.position, critter.homePosition)) ||
            curbedMove(critter, move)) {
            continue;
        }
        if (move.priority > bestPriority) {
            bestPriority = move.priority;
            best = i;
        }
    }
    if (best.has_value() || !patterns) {
        return best;
    }
    // CritterBossAI falls back to TAUNT below rateScale 0.8, then READY. These
    // are not competing priorities: Skorne gives both 512, so ranking them together
    // always picked the earlier, silent READY. CritterFindMoveType chooses the most
    // overdue eligible move, measuring cooldown from the previous animation's end.
    for (const s32 type : {MoveDefinition::kTaunt, MoveDefinition::kReady}) {
        if (type == MoveDefinition::kTaunt && attackRate(critter) >= 0.8f) {
            continue;
        }
        f32 earliest = 0.0f;
        for (usize i = 0; i < data.moves().size(); ++i) {
            const MoveDefinition& move = data.moves()[i];
            if (move.type != type || (move.flags & 4U) != 0) {
                continue;
            }
            const f32 remaining =
                move.cooldown > 0.0f ? critter.moveTimes[i] + move.cooldown - critter.age : 0.0f;
            if (type == MoveDefinition::kTaunt && remaining > 0.0f) {
                continue;
            }
            if (!best.has_value() || remaining < earliest) {
                best = i;
                earliest = remaining;
            }
        }
        if (best.has_value()) {
            return best;
        }
    }
    return std::nullopt;
}

bool Combatant::curbedMove(const Actor& critter, const MoveDefinition& move) {
    if (critter.curbSeconds <= 0.0f || !move.attack()) {
        return false;
    }
    return std::ranges::any_of(std::array{move.damage0, move.damage1}, [&](s32 index) {
        const AttackDefinition* damage = critter.definition->damage(index);
        return damage != nullptr && (damage->behaviorFlags & AttackDefinition::kCurbed) != 0 &&
               damage->type != AttackDefinition::kProjectile;
    });
}

void Combatant::chooseMove(Actor& critter, std::span<const EnemyView> players) {
    const CritterData& data = *critter.definition;
    const MoveDefinition* current =
        critter.move >= 0 ? &data.moves()[static_cast<usize>(critter.move)] : nullptr;
    // What is loudest cuts in: the death, a roar after enough taken, a hit's reaction.
    const auto cutIn = [&](std::optional<usize> index) {
        if (!index.has_value()) {
            return false;
        }
        const MoveDefinition& candidate = data.moves()[*index];
        // CritterFindMoveType uses the authored cooldown for reactions too.
        // In particular Lich ROAR has a 20-second cooldown: damage still counts
        // during it, but sustained fire must not continually restart the roar.
        if (candidate.type != MoveDefinition::kDeath && candidate.cooldown > 0.0f &&
            critter.age < critter.moveTimes[*index] + candidate.cooldown) {
            return false;
        }
        if (current != nullptr && !critter.moveDone && !candidate.interrupts(*current) &&
            candidate.type != MoveDefinition::kDeath) {
            return false;
        }
        if (startMove(critter, *index)) {
            critter.pattern = -1;
            critter.cooldowns[*index] = data.moves()[*index].cooldown;
            return true;
        }
        return false;
    };
    // A hit shoves it by its flags, dead or alive, a golem less and a boss never.
    u32 flags = critter.hurtFlags;
    if (critter.hurtPending >= 1.0f) {
        f32 scale = 0.0f;
        if (critter.state == State::Dying) {
            scale = kDeathPush;
        } else if ((flags & kHardPush) != 0) {
            scale = kHardPushScale;
        } else if ((flags & kKnockedBack) != 0) {
            scale = kKnockBackPush;
        } else if ((flags & kShaken) != 0) {
            scale = kShakePush;
        }
        scale -= critter.stock->definition.knockbackReduction;
        if (critter.stock->definition.kind != CombatantKind::Boss && scale > 0.0f) {
            critter.push += critter.hurtDirection * scale;
            if (const f32 magnitude = glm::length(critter.push); magnitude > kMostPush) {
                critter.push *= kMostPush / magnitude;
            }
        }
    }
    // Consume the hit's shove before clearing reaction requests. Even during a flinch,
    // a new impact can push the body without restarting its animation.
    if (critter.sinceHurt > kRoarMemory ||
        (current != nullptr && (current->type == MoveDefinition::kRoar || current->reaction()))) {
        critter.roarOwed = 0.0f;
        critter.hurtPending = 0.0f;
        critter.hurtFlags = 0;
        flags = 0;
    }
    if (critter.state == State::Dying) {
        critter.hurtPending = 0.0f;
        critter.hurtFlags = 0;
        if (current == nullptr || current->type != MoveDefinition::kDeath) {
            if (!cutIn(data.moveOfType(MoveDefinition::kDeath))) {
                critter.moveDone = true;
            }
        }
        return;
    }
    if (critter.forcedPattern) {
        return; // The parent's pattern step, not this branch's end frame, advances the move.
    }
    // Only a hit flagged to do so moves it off what it is doing: knocked down, knocked back,
    // a roar once enough is taken, then a flinch. Rejected hit reactions are not queued;
    // an authored link takes precedence. A roar can be retried while its damage remains.
    const bool hurt = critter.hurtPending >= 1.0f;
    critter.hurtPending = 0.0f;
    critter.hurtFlags = 0;
    const bool linked = current != nullptr && current->link >= 0;
    bool reacted = linked;
    if (hurt && !reacted && (flags & kKnockOver) != 0) {
        reacted = cutIn(data.moveOfType(MoveDefinition::kKnockDown));
    }
    if (hurt && !reacted && (flags & (kKnockOver | kKnockedBack)) != 0) {
        reacted = cutIn(data.moveOfType(MoveDefinition::kKnockBack));
    }
    if (!reacted && critter.roarOwed >= roarThreshold(m_scales.players) &&
        cutIn(data.moveOfType(MoveDefinition::kRoar))) {
        critter.roarOwed = 0.0f;
        reacted = true;
    }
    if (hurt && !reacted && (flags & kShaken) != 0) {
        cutIn(data.moveOfType(MoveDefinition::kHitReact));
    }
    // A move plays out, then what it links to, then whatever is best.
    if (current != nullptr && !critter.moveDone) {
        return;
    }
    if (current != nullptr && current->link >= 0 &&
        startMove(critter, static_cast<usize>(current->link))) {
        return;
    }
    // Asked to roar, it does so before anything else; held, it keeps to its stance.
    if (critter.roarWanted) {
        critter.pattern = -1;
        critter.roarWanted = false;
        if (const auto bellow = data.moveOfType(MoveDefinition::kRoar);
            bellow.has_value() && startMove(critter, *bellow)) {
            return;
        }
    }
    if (critter.held) {
        critter.pattern = -1;
        if (const auto ready = data.moveOfType(MoveDefinition::kReady); ready.has_value()) {
            startMove(critter, *ready);
        }
        return;
    }
    if (critter.stock->definition.selection == CombatantDefinition::Selection::Patterns &&
        choosePatternAttack(critter, players)) {
        return;
    }
    if (const auto next = bestMove(critter, players); next.has_value()) {
        startMove(critter, *next);
        if (data.moves()[*next].type == MoveDefinition::kStepToPoint) {
            if (const EnemyView* destination = viewOf(players, critter.target)) {
                // CritterLookForReady supplies CritterGetTarget's player position.
                critter.stepTarget = destination->position;
            }
        }
        critter.cooldowns[*next] = data.moves()[*next].cooldown;
        return;
    }
    if (const auto ready = data.moveOfType(MoveDefinition::kReady); ready.has_value()) {
        startMove(critter, *ready);
    }
}

} // namespace gdl::game
