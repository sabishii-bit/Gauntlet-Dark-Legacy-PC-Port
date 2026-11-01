#include <algorithm>
#include <iterator>
#include <utility>

#include "game/critters/Combatant.h"

namespace gdl::game {
namespace {
template <class T> void append(std::vector<T>& destination, std::vector<T>& source) {
    destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                       std::make_move_iterator(source.end()));
    source.clear();
}
} // namespace

const Combatant* Combatant::child(s32 id) const {
    for (const auto& part : children()) {
        if (part->id() == id) {
            return part.get();
        }
    }
    return nullptr;
}

void Combatant::inheritBodyPose() {
    if (m_sim.actor.parent != nullptr && m_sim.actor.branch.has_value()) {
        TreePose composed = m_sim.actor.parent->m_sim.actor.pose;
        composed.overlaySubtree(m_sim.actor.pose, *m_sim.actor.branch);
        m_sim.actor.pose = std::move(composed);
    }
}

void Combatant::synchronizeChild() {
    m_sim.actor.smoothValid = false;
    const Actor& parent = m_sim.actor.parent->m_sim.actor;
    if (m_sim.actor.moveEffect) {
        CombatCue stop;
        stop.critter = m_sim.id;
        stop.stopMoveEffect = true;
        m_sim.cues.push_back(stop);
        m_sim.actor.moveEffect = false;
    }
    m_sim.actor.move = -1;
    m_sim.actor.pattern = -1;
    m_sim.actor.forcedPattern = false;
    m_sim.actor.moveDone = true;
    m_sim.actor.player = parent.player;
    m_sim.actor.pose = parent.pose;
    m_sim.actor.areas.clear();
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
    append(m_sim.cues, part.m_sim.cues);
    append(m_sim.blows, part.m_sim.blows);
    append(m_sim.grabs, part.m_sim.grabs);
    append(m_sim.losses, part.m_sim.losses);
    append(m_sim.spews, part.m_sim.spews);
    append(m_sim.shots, part.m_sim.shots);
    append(m_sim.arenaActivations, part.m_sim.arenaActivations);
}

void Combatant::updateHitFlashes(Actor& actor, s32 ticks) {
    actor.flashTicks = std::max(actor.flashTicks - ticks, 0);
    for (auto& node : actor.hitNodes) {
        node.flashTicks = std::max(node.flashTicks - ticks, 0);
    }
}

void Combatant::updateChildren(s32 ticks, f32 seconds, std::span<const EnemyView> players) {
    if (children().empty()) {
        return;
    }
    constexpr s32 kSync = 1;
    const bool independent = alive() && !m_sim.actor.held &&
                             (moveType() == kSync || moveType() == MoveDefinition::kReady);
    bool busy = false;
    bool intact = true;
    bool anyAlive = false;
    for (auto& part : children()) {
        // Parts share the encounter's cumulative contact ledger. The root owns
        // updates through this transaction, so no mutable pointer is hidden in
        // the child's read-only parent binding or borrowed across rewind.
        part->m_sim.pressure = m_sim.pressure;
        Actor& actor = part->m_sim.actor;
        actor.position = position();
        actor.yaw = yaw();
        actor.scale = scale();
        actor.curbSeconds = m_sim.actor.curbSeconds;
        const bool forced =
            alive() && m_sim.actor.pattern >= 0 &&
            static_cast<usize>(m_sim.actor.pattern) < part->data()->patterns().size();
        if (part->dying()) {
            part->update(ticks, seconds, players);
        } else if (forced) {
            // CritterMoveSetup asks GetTargetSub in parent-fallback mode. A body can
            // begin its pattern after this frame's independent head roster was pruned.
            if (actor.target < 0) {
                actor.target = m_sim.actor.target;
                actor.targetDistance = m_sim.actor.targetDistance;
            }
            const auto& pattern = part->data()->patterns()[static_cast<usize>(m_sim.actor.pattern)];
            if (m_sim.actor.patternStep < pattern.moves.size()) {
                const auto step = static_cast<usize>(pattern.moves[m_sim.actor.patternStep]);
                if (!actor.forcedPattern || actor.pattern != m_sim.actor.pattern ||
                    actor.patternStep != m_sim.actor.patternStep) {
                    part->startMove(actor, step, false);
                    actor.pattern = m_sim.actor.pattern;
                    actor.patternStep = m_sim.actor.patternStep;
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
        m_sim.pressure = part->m_sim.pressure;
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
    m_sim.actor.childrenIntact = intact;
    // With its last head dead the body falls with it (ProcessCritter: its health set under
    // nought, then CritterKill).
    if (alive() && !anyAlive) {
        loseHealth(m_sim.actor.health + 1.0f);
        return;
    }
    if (independent && busy && moveType() != kSync && m_sim.actor.pattern < 0) {
        if (const auto sync = data()->moveOfType(kSync)) {
            startMove(m_sim.actor, *sync);
            for (auto& part : children()) {
                part->inheritBodyPose();
            }
        }
    }
}
} // namespace gdl::game
