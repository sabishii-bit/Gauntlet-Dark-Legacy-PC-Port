#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/Combatant.h"

namespace gdl::game {

f32 Combatant::attackRate(const Actor& critter) {
    return 0.5f + 4.5f * (1.0f - critter.health / (1.0f + critter.maxHealth));
}

s32 Combatant::attackTarget(const Actor& critter, const TargetCriteria& criteria,
                            std::span<const EnemyView> players, bool fallback) {
    for (const Actor* owner = &critter; owner != nullptr;
         owner = fallback && owner->parent != nullptr ? &owner->parent->m_actor : nullptr) {
        const Actor& source = *owner;
        if (source.blindTicks > 0) {
            return -1;
        }
        const Vec3 home = source.position - source.homePosition;
        if (criteria.maxHomeDistance > 0 &&
            glm::length(Vec2{home.x, home.z}) > criteria.maxHomeDistance) {
            return -1;
        }
        const bool phase =
            criteria.allowsPhase(attackRate(source), glm::length(Vec2{home.x, home.z}));
        if (!phase && !fallback) {
            return -1;
        }
        s32 target = -1;
        f32 nearest = std::numeric_limits<f32>::max();
        for (const Target& candidate : source.targets) {
            const EnemyView* player = viewOf(players, candidate.player);
            if (player == nullptr || player->hidden) {
                continue;
            }
            // Retargeting uses the geometry gathered before the whole family
            // chooses moves, without repeating sight's vertical gate. Updating
            // the body first must not move or turn its child's selection window.
            const f32 distance = candidate.distance;
            const f32 heading = source.targetYaw + criteria.yaw;
            const f32 dot =
                glm::dot(candidate.direction, Vec2{std::sin(heading), std::cos(heading)});
            const bool eligible = phase && distance >= criteria.minDistance &&
                                  (criteria.maxDistance <= 0 || distance <= criteria.maxDistance) &&
                                  dot >= criteria.minDot;
            // ReCalcTarget's rejected scores still rank MoveSetup's fallback. A player
            // too near outranks one too far, then one outside the cone, then the rate gate.
            f32 score = distance * candidate.inverseAnger;
            if (!phase) {
                score = 1.2e21f;
            } else if (distance < criteria.minDistance) {
                score = 1.01e21f;
            } else if (criteria.maxDistance > 0 && distance > criteria.maxDistance) {
                score = 1.02e21f;
            } else if (!eligible) {
                score = 1.1e21f;
            }
            if (score < nearest && (eligible || fallback)) {
                nearest = score;
                target = player->player;
            }
        }
        if (target >= 0) {
            return target;
        }
    }
    return -1;
}

bool Combatant::choosePatternAttack(Actor& critter, std::span<const EnemyView> players) {
    const CritterData& data = *critter.definition;
    const auto available = [&](s32 index) {
        if (index < 0 || static_cast<usize>(index) >= data.moves().size()) {
            return false;
        }
        const MoveDefinition& move = data.moves()[static_cast<usize>(index)];
        return !curbedMove(critter, move) &&
               critter.stock->tree->findSequence(move.anim).has_value();
    };
    const s32 previousPattern = critter.pattern;
    const auto canStart = [&](s32 index) {
        return critter.move < 0 || critter.moveDone ||
               (index != critter.move &&
                (data.moves()[static_cast<usize>(critter.move)].hold <= 0 ||
                 data.moves()[static_cast<usize>(index)].priority >= MoveDefinition::kCutsIn) &&
                data.moves()[static_cast<usize>(index)].interrupts(
                    data.moves()[static_cast<usize>(critter.move)]));
    };
    if (previousPattern >= 0) {
        const auto& pattern = data.patterns()[static_cast<usize>(previousPattern)];
        const usize next = critter.patternStep + 1;
        // The chain itself authorizes subsequent moves, even if their individual
        // health/range gates now fail. Legend-item curbs still cancel it.
        if (next < pattern.moves.size() && available(pattern.moves[next])) {
            if (canStart(pattern.moves[next]) &&
                startMove(critter, static_cast<usize>(pattern.moves[next]), false)) {
                critter.patternStep = next;
            }
            return true; // A queued continuation retains the chain until it can transition.
        }
    }
    f32 oldest = std::numeric_limits<f32>::max();
    s32 patternChoice = -1;
    s32 moveChoice = -1;
    s32 playerChoice = -1;
    for (usize i = 0; i < data.patterns().size(); ++i) {
        const AttackPattern& pattern = data.patterns()[i];
        constexpr u32 kDisabled = 0x1000;
        if (static_cast<s32>(i) == previousPattern || pattern.moves.empty() ||
            (pattern.flags & kDisabled) != 0 ||
            ((pattern.flags & 2U) != 0 && !critter.childrenIntact) ||
            !available(pattern.moves.front()) ||
            critter.age < critter.patternTimes[i] + pattern.cooldown) {
            continue;
        }
        const s32 target = attackTarget(critter, pattern.target, players);
        if (target >= 0 && critter.patternTimes[i] < oldest) {
            oldest = critter.patternTimes[i];
            patternChoice = static_cast<s32>(i);
            moveChoice = pattern.moves.front();
            playerChoice = target;
        }
    }
    for (usize i = 0; i < data.moves().size(); ++i) {
        const MoveDefinition& move = data.moves()[i];
        constexpr u32 kLinkedOnly = 4;
        constexpr u32 kRequiresNode = 0x10;
        if (static_cast<s32>(i) == critter.move || !move.attack() ||
            ((move.flags & 2U) != 0 && !critter.childrenIntact) ||
            (move.flags & kLinkedOnly) != 0 || !available(static_cast<s32>(i))) {
            continue;
        }
        const auto hasNode = [&](const MoveDefinition& m) {
            return nodeAvailable(critter, m.colnode);
        };
        if ((move.flags & kRequiresNode) != 0 &&
            (!hasNode(move) ||
             (move.link >= 0 && (static_cast<usize>(move.link) >= data.moves().size() ||
                                 !hasNode(data.moves()[static_cast<usize>(move.link)]))))) {
            continue;
        }
        // Holding a player, it takes the first grab move there is, whatever its cooldown
        // or target (CritterLookForCriticalMove's GRABAGAIN); the shipped grabs let go
        // within their own animation, so this is reached only by a grab cut short.
        if (critter.grabbed >= 0 && move.type == MoveDefinition::kGrab) {
            patternChoice = -1;
            moveChoice = static_cast<s32>(i);
            playerChoice = critter.grabbed;
            break;
        }
        if (move.cooldown > 0.0f && critter.age < critter.moveTimes[i] + move.cooldown) {
            continue;
        }
        const s32 target = attackTarget(critter, move.target, players);
        if (target < 0) {
            continue;
        }
        if (critter.moveTimes[i] < oldest ||
            (patternChoice < 0 &&
             (moveChoice < 0 || move.interrupts(data.moves()[static_cast<usize>(moveChoice)])))) {
            oldest = critter.moveTimes[i];
            patternChoice = -1;
            moveChoice = static_cast<s32>(i);
            playerChoice = target;
        }
    }
    if (moveChoice < 0) {
        if (critter.moveDone) {
            critter.pattern = -1;
        }
        return false;
    }
    // Selection is evaluated during the animation. A rejected transition must
    // not consume cooldowns, retarget the active attack, or advance its pattern.
    if (!canStart(moveChoice)) {
        return true;
    }
    if (!startMove(critter, static_cast<usize>(moveChoice), patternChoice < 0)) {
        return false;
    }
    critter.target = playerChoice;
    critter.moveTarget = playerChoice;
    const auto selected = std::ranges::find(critter.targets, playerChoice, &Target::player);
    if (selected != critter.targets.end()) {
        critter.targetDistance = selected->distance;
    } else if (const EnemyView* target = viewOf(players, playerChoice)) {
        // GRABAGAIN can retain a held player absent from this frame's sight roster.
        const Vec3 delta = target->position - targetingOrigin(critter);
        critter.targetDistance = glm::length(Vec2{delta.x, delta.z});
    }
    critter.pattern = patternChoice;
    critter.patternStep = 0;
    if (patternChoice >= 0) {
        critter.patternTimes[static_cast<usize>(patternChoice)] = critter.age;
    }
    return true;
}

} // namespace gdl::game
