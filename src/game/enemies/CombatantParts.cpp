#include <algorithm>
#include <iterator>
#include <utility>

#include "game/enemies/Combatant.h"

namespace gdl::game {
namespace {
template <class T> void append(std::vector<T>& destination, std::vector<T>& source) {
    destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                       std::make_move_iterator(source.end()));
    source.clear();
}
} // namespace

const Combatant* Combatant::child(s32 id) const {
    for (const auto& part : m_children) {
        if (part->id() == id) {
            return part.get();
        }
    }
    return nullptr;
}

void Combatant::inheritBodyPose() {
    if (m_actor.parent != nullptr && m_actor.branch.has_value()) {
        TreePose composed = m_actor.parent->m_actor.pose;
        composed.overlaySubtree(m_actor.pose, *m_actor.branch);
        m_actor.pose = std::move(composed);
    }
}

void Combatant::synchronizeChild() {
    m_actor.smoothValid = false;
    const Actor& parent = m_actor.parent->m_actor;
    if (m_actor.moveEffect) {
        CombatCue stop;
        stop.critter = m_id;
        stop.stopMoveEffect = true;
        m_cues.push_back(stop);
        m_actor.moveEffect = false;
    }
    m_actor.move = -1;
    m_actor.pattern = -1;
    m_actor.forcedPattern = false;
    m_actor.moveDone = true;
    m_actor.player = parent.player;
    m_actor.pose = parent.pose;
    m_actor.areas.clear();
}

void Combatant::aimGaze(Actor& critter, f32 seconds, std::span<const EnemyView> players) {
    const CritterData& data = *critter.definition;
    const MoveDefinition* move =
        critter.move >= 0 ? &data.moves()[static_cast<usize>(critter.move)] : nullptr;
    if (move != nullptr &&
        (move->type == MoveDefinition::kStart || move->type == MoveDefinition::kInit)) {
        return;
    }
    constexpr u32 kHeadHeld = 1; ///< the move keeps the head to its animation
    const bool held = (move != nullptr &&
                       (move->type == MoveDefinition::kDeath || (move->flags & kHeadHeld) != 0)) ||
                      critter.frozenTicks > 0 || critter.blindTicks > 0;
    std::optional<Vec3> target;
    if (!held) {
        if (const EnemyView* view = viewOf(players, critter.moveTarget)) {
            target = view->position + Vec3{0.0f, 0.5f * view->height, 0.0f};
        }
    }
    critter.gaze.aim(critter.pose, *critter.stock->tree, modelTransform(critter), data.looks(),
                     target, seconds);
}

void Combatant::collectChildEvents(Combatant& part) {
    append(m_cues, part.m_cues);
    append(m_blows, part.m_blows);
    append(m_grabs, part.m_grabs);
    append(m_losses, part.m_losses);
    append(m_spews, part.m_spews);
    append(m_shots, part.m_shots);
    append(m_arenaActivations, part.m_arenaActivations);
}

void Combatant::updateHitFlashes(Actor& actor, s32 ticks) {
    actor.flashTicks = std::max(actor.flashTicks - ticks, 0);
    for (auto& node : actor.hitNodes) {
        node.flashTicks = std::max(node.flashTicks - ticks, 0);
    }
}

void Combatant::updateChildren(s32 ticks, f32 seconds, std::span<const EnemyView> players) {
    if (m_children.empty()) {
        return;
    }
    constexpr s32 kSync = 1;
    const bool independent =
        alive() && !m_actor.held && (moveType() == kSync || moveType() == MoveDefinition::kReady);
    bool busy = false;
    bool intact = true;
    bool anyAlive = false;
    for (auto& part : m_children) {
        Actor& actor = part->m_actor;
        actor.position = position();
        actor.yaw = yaw();
        actor.scale = scale();
        actor.curbSeconds = m_actor.curbSeconds;
        const bool forced = alive() && m_actor.pattern >= 0 &&
                            static_cast<usize>(m_actor.pattern) < part->data()->patterns().size();
        if (part->dying()) {
            part->update(ticks, seconds, players);
        } else if (forced) {
            // CritterMoveSetup asks GetTargetSub in parent-fallback mode. A body can
            // begin its pattern after this frame's independent head roster was pruned.
            if (actor.target < 0) {
                actor.target = m_actor.target;
                actor.targetDistance = m_actor.targetDistance;
            }
            const auto& pattern = part->data()->patterns()[static_cast<usize>(m_actor.pattern)];
            if (m_actor.patternStep < pattern.moves.size()) {
                const auto step = static_cast<usize>(pattern.moves[m_actor.patternStep]);
                if (!actor.forcedPattern || actor.pattern != m_actor.pattern ||
                    actor.patternStep != m_actor.patternStep) {
                    part->startMove(actor, step, false);
                    actor.pattern = m_actor.pattern;
                    actor.patternStep = m_actor.patternStep;
                } else if (actor.moveDone) {
                    // The step's move is asked for again every frame, so it starts over each
                    // time it ends while the body's step lasts (CritterAnimate, AnimateTree's
                    // restart-when-done), its harm rearmed (CritterMoveSetup).
                    part->startMove(actor, step, false);
                }
                actor.forcedPattern = true;
                part->update(ticks, seconds, players);
            } else {
                part->synchronizeChild();
            }
        } else if (independent) {
            if (actor.forcedPattern) {
                actor.pattern = -1;
                actor.forcedPattern = false;
            }
            part->update(ticks, seconds, players);
        } else {
            actor.age += seconds;
            part->synchronizeChild();
            part->aimGaze(actor, seconds, players); // its head still turns (CritterLookAtPlayer)
        }
        intact &= part->alive();
        anyAlive |= part->alive();
        // A head at an attack of its own, or in a pattern, keeps the body in SYNC
        // (CritterBossAI's linked children); a flinch or a roar does not.
        busy |=
            part->alive() && !actor.moveDone &&
            (actor.pattern >= 0 ||
             (actor.move >= 0 && part->data()->moves()[static_cast<usize>(actor.move)].attack()));
        collectChildEvents(*part);
    }
    m_actor.childrenIntact = intact;
    // With its last head dead the body falls with it (ProcessCritter: its health set under
    // nought, then CritterKill).
    if (alive() && !anyAlive) {
        loseHealth(m_actor.health + 1.0f);
        return;
    }
    if (independent && busy && moveType() != kSync && m_actor.pattern < 0) {
        if (const auto sync = data()->moveOfType(kSync)) {
            startMove(m_actor, *sync);
            for (auto& part : m_children) {
                part->inheritBodyPose();
            }
        }
    }
}
} // namespace gdl::game
