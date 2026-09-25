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
                            std::span<const EnemyView> players) {
    if (critter.blindTicks > 0) {
        return -1;
    }
    const Vec3 home = critter.position - critter.homePosition;
    if (!criteria.allowsPhase(attackRate(critter), glm::length(Vec2{home.x, home.z}))) {
        return -1;
    }
    s32 target = -1;
    f32 nearest = std::numeric_limits<f32>::max();
    for (const EnemyView& player : players) {
        const Vec3 delta = player.position - critter.position;
        const f32 distance = glm::length(Vec2{delta.x, delta.z});
        const f32 bearing = std::atan2(delta.x, delta.z) - critter.yaw;
        if (!player.hidden && distance < nearest &&
            critter.definition->sight().allows(distance, 0.0f, delta.y) &&
            criteria.allows(distance, bearing, delta.y)) {
            nearest = distance;
            target = player.player;
        }
    }
    return target;
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
    if (previousPattern >= 0) {
        const auto& pattern = data.patterns()[static_cast<usize>(previousPattern)];
        const usize next = critter.patternStep + 1;
        // The chain itself authorizes subsequent moves, even if their individual
        // health/range gates now fail. Legend-item curbs still cancel it.
        if (next < pattern.moves.size() && available(pattern.moves[next]) &&
            startMove(critter, static_cast<usize>(pattern.moves[next]), false)) {
            critter.patternStep = next;
            return true;
        }
        critter.pattern = -1;
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
            (move.flags & kLinkedOnly) != 0 || !available(static_cast<s32>(i)) ||
            (move.cooldown > 0.0f && critter.age < critter.moveTimes[i] + move.cooldown)) {
            continue;
        }
        const auto hasNode = [&](const MoveDefinition& m) {
            return critter.stock->tree->findNode(m.colnode).has_value();
        };
        if ((move.flags & kRequiresNode) != 0 &&
            (!hasNode(move) ||
             (move.link >= 0 && (static_cast<usize>(move.link) >= data.moves().size() ||
                                 !hasNode(data.moves()[static_cast<usize>(move.link)]))))) {
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
    if (moveChoice < 0 || !startMove(critter, static_cast<usize>(moveChoice), patternChoice < 0)) {
        return false;
    }
    critter.target = playerChoice;
    if (const EnemyView* target = viewOf(players, playerChoice)) {
        const Vec3 delta = target->position - critter.position;
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
