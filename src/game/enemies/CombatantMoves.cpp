#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
    critter.moveTarget = -1;
    critter.moveDone = false;
    critter.finishedSeconds = 0.0f;
    critter.struckThisMove.clear();
    critter.soundsGiven = 0;
    critter.shotFrame = -1;
    critter.attackTarget.reset();
    critter.stepTarget.reset();
    critter.smoothValid = false;
    critter.player.start(critter.stock->tree->sequences[*sequence], *sequence);
    // CritterAnimate uses the body's sequence index, falling back to zero for
    // shorter auxiliary trees (the Lich's FLIES has just one repeating sequence).
    for (usize j = 0; j < critter.attachments.size(); ++j) {
        auto& attachment = critter.attachments[j];
        const auto& tree = *critter.stock->attachments[j].tree;
        const u32 selected = *sequence < tree.sequences.size() ? *sequence : 0;
        if (!attachment.player.playing() || attachment.player.sequence() != selected ||
            !tree.sequences[selected].repeats) {
            attachment.player.start(tree.sequences[selected], selected);
        }
        attachment.pose.evaluate(tree, selected, attachment.player.frame());
    }
    if (recordUse) {
        critter.moveTimes[index] =
            critter.age + static_cast<f32>(std::max(critter.player.frameCount() - 2, 0)) / 30.0f;
    }
    critter.pose.evaluate(*critter.stock->tree, *sequence, 0.0f);
    return true;
}

void Combatant::chooseTarget(Actor& critter, std::span<const EnemyView> players) {
    critter.targets.clear();
    critter.targetYaw = critter.yaw;
    const bool boss = critter.definition->kind() == CombatantKind::Boss;
    const TargetCriteria& sight = critter.definition->sight();
    const Vec3 origin = targetingOrigin(critter);
    for (const EnemyView& view : players) {
        if (critter.state != State::Active || view.hidden || (!boss && view.invisible) ||
            view.player < 0 || static_cast<usize>(view.player) >= kPlayerSlots) {
            continue;
        }
        // Sight measures the player's collision centre against the body's rotated
        // TYPE origin, not either actor's feet. Attack retargeting keeps this geometry.
        const Vec3 target =
            view.position + Vec3{0, view.collisionHeight.value_or(0.5f * view.height), 0};
        const Vec3 delta = target - origin;
        const f32 distance = glm::length(Vec2{delta.x, delta.z});
        const Vec2 direction = distance > 0 ? Vec2{delta.x, delta.z} / distance : Vec2{0};
        const f32 bearing = yawBetween(origin, target) - critter.yaw;
        if (!sight.allows(distance, bearing, delta.y) ||
            !sight.allowsPhase(attackRate(critter),
                               flatDistance(critter.position, critter.homePosition))) {
            continue;
        }
        const f32 score = targetScore(critter, target);
        // On its round of the lookouts it takes only a player within its placement's sight
        // (CritterGetSingleTargetPlayer's visrad).
        if (critter.patrol.active() && critter.patrol.sight() > 0.0f &&
            score > critter.patrol.sight()) {
            continue;
        }
        constexpr f32 kRecentlyHitPenalty = 1000.0f;
        const f32 anger = boss ? inverseAnger(critter, static_cast<usize>(view.player)) : 1.0f;
        critter.targets.push_back({view.player, distance,
                                   score * anger * (view.recentlyHit ? kRecentlyHitPenalty : 1),
                                   anger, direction});
    }
    std::ranges::sort(critter.targets, [](const Target& a, const Target& b) {
        return a.score != b.score ? a.score < b.score : a.player < b.player;
    });
    if (!boss && critter.targets.size() > 1) {
        critter.targets.resize(1);
    }
    selectFirstTarget(critter);
    // A player found ends the round for good.
    if (critter.target >= 0 && critter.patrol.active()) {
        critter.patrol.end();
        critter.patrolAim.reset();
    }
}

void Combatant::selectFirstTarget(Actor& critter) {
    critter.target = critter.targets.empty() ? -1 : critter.targets.front().player;
    critter.targetDistance = critter.targets.empty() ? 100000.0f : critter.targets.front().distance;
}

f32 Combatant::targetClock(const Actor& critter) {
    return critter.parent != nullptr ? critter.parent->m_actor.age : critter.age;
}

f32 Combatant::inverseAnger(const Actor& critter, usize player) {
    const PlayerDamage& memory = critter.playerDamage[player];
    const f32 now = targetClock(critter);
    const f32 dealt = now - memory.dealtTime > kAngerMemory ? 0 : memory.dealt;
    const f32 received = now - memory.receivedTime > kAngerMemory ? 0 : memory.received;
    return dealt < 1.0f ? 11.0f : std::clamp(received / dealt, 0.01f, 10.0f);
}

void Combatant::rememberDamage(f32& total, f32& lastTime, f32 now, f32 amount) {
    if (now - lastTime > kAngerMemory) {
        total = 0;
    }
    total += amount;
    lastTime = now;
}

void Combatant::chooseFamilyTargets(std::span<const EnemyView> players) {
    chooseTarget(m_actor, players);
    std::array<s32, kPlayerSlots> counts{};
    const auto count = [&](const Actor& actor) {
        for (const Target& target : actor.targets) {
            ++counts[static_cast<usize>(target.player)];
        }
    };
    count(m_actor);
    for (auto& child : m_children) {
        child->m_actor.position = m_actor.position;
        child->m_actor.yaw = m_actor.yaw;
        chooseTarget(child->m_actor, players);
        count(child->m_actor);
    }
    // A pattern owns the whole chain. Otherwise limit each player's candidate memberships,
    // removing the weakest child claim first, never the root's or a child's active pattern.
    if (m_actor.pattern >= 0) {
        return;
    }
    for (const Target& target : m_actor.targets) {
        s32 limit = 4;
        if (target.inverseAnger > 1) {
            limit = 2;
        } else if (target.inverseAnger > 0.75f) {
            limit = 3;
        }
        s32& assigned = counts[static_cast<usize>(target.player)];
        while (assigned > limit) {
            Actor* weakest = nullptr;
            f32 worst = 0;
            for (auto& child : m_children) {
                Actor& actor = child->m_actor;
                if (actor.pattern >= 0) {
                    continue;
                }
                for (const Target& candidate : actor.targets) {
                    if (candidate.player == target.player &&
                        (weakest == nullptr || candidate.score > worst)) {
                        weakest = &actor;
                        worst = candidate.score;
                        break;
                    }
                }
            }
            if (weakest == nullptr) {
                break;
            }
            std::erase_if(weakest->targets, [&](const Target& candidate) {
                return candidate.player == target.player;
            });
            selectFirstTarget(*weakest);
            --assigned;
        }
    }
}

Vec3 Combatant::targetingOrigin(const Actor& critter) {
    const Actor* root = &critter;
    while (root->parent != nullptr) {
        root = &root->parent->m_actor;
    }
    // Child parts inherit the root's targeting centre. A head's own mesh
    // attachment and TYPE origin do not relocate its sight window.
    return partPosition(*root, {});
}

f32 Combatant::targetScore(const Actor& critter, const Vec3& position) {
    constexpr f32 kSquarelyAhead = 0.5f;
    const Vec3 origin = targetingOrigin(critter);
    const f32 distance = flatDistance(position, origin);
    const f32 dot = std::cos(
        wrapAngle(yawBetween(origin, position) - critter.yaw - critter.definition->sight().yaw));
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
        // Ready selection calls CalcTarget again, including its vertical gate;
        // attack selection instead consumes the previously gathered target record.
        const Vec3 origin = targetingOrigin(critter);
        const Vec3 target =
            view->position + Vec3{0, view->collisionHeight.value_or(0.5f * view->height), 0};
        distance = flatDistance(target, origin);
        bearing = wrapAngle(yawBetween(origin, target) - critter.yaw);
        vertical = target.y - origin.y;
    }
    // With nobody in its sights, one on its round walks to the next lookout.
    if (view == nullptr) {
        if (const auto step = patrolStep(critter); step.has_value()) {
            return step;
        }
    }
    std::optional<usize> best;
    f32 bestScore = std::numeric_limits<f32>::max();
    for (usize i = 0; i < data.moves().size(); ++i) {
        const MoveDefinition& move = data.moves()[i];
        // CritterLookForReady searches movement before the idle fallback. Move
        // priority controls interruptions, not which eligible step it chooses.
        const bool step =
            move.type >= MoveDefinition::kStepFrom && move.type <= MoveDefinition::kStepLast;
        constexpr u32 kLinkedOnly = 4;
        if (!step || (move.flags & kLinkedOnly) != 0 ||
            (move.cooldown > 0 && critter.age < critter.moveTimes[i] + move.cooldown)) {
            continue;
        }
        if (view == nullptr ||
            (move.type == MoveDefinition::kStepToPoint && critter.moveTarget < 0)) {
            continue;
        }
        if (!move.target.allows(distance, bearing, vertical) ||
            !move.target.allowsPhase(attackRate(critter),
                                     flatDistance(critter.position, critter.homePosition)) ||
            curbedMove(critter, move)) {
            continue;
        }
        constexpr f32 kSquarelyAhead = 0.5f;
        const f32 dot = std::cos(bearing - move.target.yaw);
        const f32 score = dot > kSquarelyAhead ? distance / dot : 2.0f * distance;
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best.has_value()) {
        return best;
    }
    // Boss and ordinary AI both fall back to TAUNT below rateScale 0.8, then READY.
    // CritterFindMoveType chooses the most overdue eligible move, measuring
    // cooldown from the previous animation's end.
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
        reacted = cutIn(data.moveOfType(MoveDefinition::kHitReact));
    }
    if (reacted && !linked) {
        return;
    }
    const auto transition = [&](usize index) {
        if (index >= data.moves().size()) {
            return false;
        }
        if (current != nullptr && !critter.moveDone &&
            ((current->hold > 0 && data.moves()[index].priority < MoveDefinition::kCutsIn) ||
             static_cast<s32>(index) == critter.move ||
             !data.moves()[index].interrupts(*current))) {
            return false;
        }
        return startMove(critter, index);
    };
    // Linked continuations outrank a new choice, but still obey the current
    // animation's interruption policy until it finishes.
    if (current != nullptr && current->link >= 0) {
        transition(static_cast<usize>(current->link));
        return;
    }
    // Asked to roar, it does so before anything else; held, it keeps to its stance.
    if (critter.roarWanted) {
        if (const auto bellow = data.moveOfType(MoveDefinition::kRoar);
            bellow.has_value() && transition(*bellow)) {
            critter.pattern = -1;
            critter.roarWanted = false;
            return;
        }
    }
    if (critter.held) {
        critter.pattern = -1;
        if (const auto ready = data.moveOfType(MoveDefinition::kReady); ready.has_value()) {
            transition(*ready);
        }
        return;
    }
    // The ordinary fighters search defensive moves before offensive attacks.
    if (critter.stock->definition.selection == CombatantDefinition::Selection::Priority) {
        std::optional<usize> block;
        for (usize i = 0; i < data.moves().size(); ++i) {
            const MoveDefinition& move = data.moves()[i];
            constexpr u32 kLinkedOnly = 4;
            constexpr u32 kRequiresNode = 0x10;
            if (move.type != MoveDefinition::kBlock || (move.flags & kLinkedOnly) != 0 ||
                ((move.flags & 2U) != 0 && !critter.childrenIntact) ||
                (move.cooldown > 0 && critter.age < critter.moveTimes[i] + move.cooldown) ||
                !critter.stock->tree->findSequence(move.anim).has_value()) {
                continue;
            }
            if ((move.flags & kRequiresNode) != 0 &&
                (!nodeAvailable(critter, move.colnode) ||
                 (move.link >= 0 &&
                  (static_cast<usize>(move.link) >= data.moves().size() ||
                   !nodeAvailable(critter,
                                  data.moves()[static_cast<usize>(move.link)].colnode))))) {
                continue;
            }
            const s32 player = attackTarget(critter, move.target, players);
            const EnemyView* target = player >= 0 ? viewOf(players, player) : nullptr;
            if (target != nullptr && target->blockableAttack) {
                block = i; // The last eligible BLOCK in the authored table wins.
            }
        }
        if (block.has_value()) {
            transition(*block); // A refused interrupt still owns this frame's request.
            return;
        }
    }
    // Golem/general/gargoyle AI and boss AI both search attacks before ready steps,
    // using the same oldest-use scheduler and animation interruption rules.
    if (choosePatternAttack(critter, players)) {
        return;
    }
    if (const auto next = bestMove(critter, players); next.has_value()) {
        if (!transition(*next)) {
            return;
        }
        if (data.moves()[*next].type == MoveDefinition::kStepToPoint) {
            if (const EnemyView* destination = viewOf(players, critter.target)) {
                // CritterLookForReady supplies CritterGetTarget's player position.
                critter.stepTarget = destination->position;
            }
        }
        return;
    }
    if (const auto ready = data.moveOfType(MoveDefinition::kReady); ready.has_value()) {
        transition(*ready);
    }
}

} // namespace gdl::game
