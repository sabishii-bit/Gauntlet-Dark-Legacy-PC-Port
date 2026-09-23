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
constexpr f32 kKnockScale = 20.0f;
constexpr f32 kMostPush = 40.0f;
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
    const CritterData& data = critter.stock->data;
    if (index >= data.moves().size()) {
        return false;
    }
    const MoveDefinition& move = data.moves()[index];
    const auto sequence = critter.stock->tree->findSequence(move.anim);
    if (!sequence.has_value()) {
        return false;
    }
    critter.move = static_cast<s32>(index);
    critter.moveDone = false;
    critter.finishedSeconds = 0.0f;
    critter.struckThisMove.clear();
    critter.soundsGiven = 0;
    critter.shotFrame = -1;
    critter.attackTarget.reset();
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
    const TargetCriteria& sight = critter.stock->data.sight();
    for (const EnemyView& view : players) {
        if (view.hidden) {
            continue;
        }
        const f32 distance = flatDistance(view.position, critter.position);
        if (!sight.allows(distance, 0.0f, view.position.y - critter.position.y)) {
            continue;
        }
        if (distance < critter.targetDistance) {
            critter.targetDistance = distance;
            critter.target = view.player;
        }
    }
}

std::optional<usize> Combatant::bestMove(const Actor& critter, std::span<const EnemyView> players) {
    const CritterData& data = critter.stock->data;
    const EnemyView* view = viewOf(players, critter.target);
    f32 distance = 100000.0f;
    f32 bearing = kPi;
    f32 vertical = 0.0f;
    if (view != nullptr) {
        distance = critter.targetDistance;
        bearing = wrapAngle(yawBetween(critter.position, view->position) - critter.yaw);
        vertical = view->position.y - critter.position.y;
    }
    std::optional<usize> best;
    s32 bestPriority = -1;
    for (usize i = 0; i < data.moves().size(); ++i) {
        const MoveDefinition& move = data.moves()[i];
        // Attacks, the steps (walks, turns and back-steps, types 48 to 63), the stance and
        // the taunt.
        const bool step =
            move.type >= MoveDefinition::kStepFrom && move.type < MoveDefinition::kStepTo;
        const bool considered = (move.attack() && critter.stock->definition.selection ==
                                                      CombatantDefinition::Selection::Priority) ||
                                step || move.type == MoveDefinition::kReady ||
                                move.type == MoveDefinition::kTaunt;
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
    return best;
}

bool Combatant::curbedMove(const Actor& critter, const MoveDefinition& move) {
    if (critter.curbSeconds <= 0.0f || !move.attack()) {
        return false;
    }
    return std::ranges::any_of(std::array{move.damage0, move.damage1}, [&](s32 index) {
        const AttackDefinition* damage = critter.stock->data.damage(index);
        return damage != nullptr && (damage->behaviorFlags & AttackDefinition::kCurbed) != 0 &&
               damage->type != AttackDefinition::kProjectile;
    });
}

void Combatant::chooseMove(Actor& critter, std::span<const EnemyView> players) {
    const CritterData& data = critter.stock->data;
    const MoveDefinition* current =
        critter.move >= 0 ? &data.moves()[static_cast<usize>(critter.move)] : nullptr;
    // What is loudest cuts in: the death, a roar after enough taken, a hit's reaction.
    const auto cutIn = [&](std::optional<usize> index) {
        if (!index.has_value() || (current != nullptr && !critter.moveDone &&
                                   data.moves()[*index].priority <= current->priority)) {
            return false;
        }
        if (startMove(critter, *index)) {
            critter.pattern = -1;
            critter.cooldowns[*index] = data.moves()[*index].cooldown;
            return true;
        }
        return false;
    };
    if (critter.state == State::Dying) {
        if (current == nullptr || current->type != MoveDefinition::kDeath) {
            if (!cutIn(data.moveOfType(MoveDefinition::kDeath))) {
                critter.moveDone = true;
            }
        }
        return;
    }
    if (critter.hurtPending >= 1.0f) {
        const bool floors = (critter.hurtFlags & EnemyHit::kFloors) != 0;
        const auto reaction =
            data.moveOfType(floors ? MoveDefinition::kKnockDown : MoveDefinition::kKnockBack);
        if (cutIn(reaction.has_value() ? reaction : data.moveOfType(MoveDefinition::kKnockBack))) {
            f32 scale = floors ? kKnockScale : 0.0f;
            scale = std::max(scale - critter.stock->definition.knockbackReduction, 0.0f);
            critter.push += critter.hurtDirection * scale;
            if (const f32 magnitude = glm::length(critter.push); magnitude > kMostPush) {
                critter.push *= kMostPush / magnitude;
            }
        }
        critter.hurtPending = 0.0f;
        critter.hurtFlags = 0;
    }
    if (critter.roarOwed >= kRoarAfter && cutIn(data.moveOfType(MoveDefinition::kRoar))) {
        critter.roarOwed = 0.0f;
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
        critter.cooldowns[*next] = data.moves()[*next].cooldown;
        return;
    }
    if (const auto ready = data.moveOfType(MoveDefinition::kReady); ready.has_value()) {
        startMove(critter, *ready);
    }
}

} // namespace gdl::game
