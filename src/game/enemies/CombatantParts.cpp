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

void Combatant::collectChildEvents(Combatant& part) {
    append(m_cues, part.m_cues);
    append(m_blows, part.m_blows);
    append(m_grabs, part.m_grabs);
    append(m_losses, part.m_losses);
    append(m_spews, part.m_spews);
    append(m_shots, part.m_shots);
    append(m_arenaActivations, part.m_arenaActivations);
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
            const auto& pattern = part->data()->patterns()[static_cast<usize>(m_actor.pattern)];
            if (m_actor.patternStep < pattern.moves.size()) {
                if (!actor.forcedPattern || actor.pattern != m_actor.pattern ||
                    actor.patternStep != m_actor.patternStep) {
                    part->startMove(actor, static_cast<usize>(pattern.moves[m_actor.patternStep]),
                                    false);
                    actor.pattern = m_actor.pattern;
                    actor.patternStep = m_actor.patternStep;
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
        }
        intact &= part->alive();
        busy |= part->alive() && !actor.moveDone && part->moveType() != MoveDefinition::kReady;
        collectChildEvents(*part);
    }
    m_actor.childrenIntact = intact;
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
