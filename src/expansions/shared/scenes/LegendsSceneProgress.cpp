#include <utility>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::touchGates(PlayerActor& actor) {
    const LegendsPortalVisitor visitor{static_cast<u8>(actor.player()), actor.position(), true,
                                       actor.radius(), actor.height()};
    for (auto& gate : m_gates) {
        auto& keys = actor.save().progress().inventory.keys;
        if (gate.state.touch(visitor, keys)) {
            if (gate.state.definition().sound != 0) {
                m_soundEvents.push_back(
                    {gate.state.definition().sound, gate.state.definition().position});
            }
            m_hiddenFixtures[gate.idleFigure] = true;
            m_hiddenFixtures[gate.openingFigure] = gate.state.definition().hidden;
            m_fixtures[gate.openingFigure].play(gate.openingSequence, ItemFigure::Playback::Once);
            m_fixtures[gate.openingFigure].capturePresentation();
        }
    }
}

void LegendsSceneAdapter::updateGates() {
    for (auto& gate : m_gates) {
        if (gate.state.activateInitially()) {
            if (gate.state.definition().sound != 0) {
                m_soundEvents.push_back(
                    {gate.state.definition().sound, gate.state.definition().position});
            }
            m_hiddenFixtures[gate.idleFigure] = true;
            m_hiddenFixtures[gate.openingFigure] = gate.state.definition().hidden;
            m_fixtures[gate.openingFigure].play(gate.openingSequence, ItemFigure::Playback::Once);
            m_fixtures[gate.openingFigure].capturePresentation();
        }
        if (gate.state.state() == LegendsGate::State::Opening &&
            m_fixtures[gate.openingFigure].finished()) {
            gate.state.finishOpening();
            if (gate.openSequence >= 0) {
                m_fixtures[gate.openingFigure].play(gate.openSequence, ItemFigure::Playback::Loop);
                m_fixtures[gate.openingFigure].capturePresentation();
            }
        }
    }
}

void LegendsSceneAdapter::synchronizeProgress(std::span<const CharacterSave* const> party) {
    LegendsProgress combined;
    for (const auto* save : party) {
        if (!save || save->character < 0 || save->character >= kClassCount) {
            continue;
        }
        const auto& progress = save->progress().legendsProgress(m_platform);
        combined.runestones |= progress.runestones;
        combined.shards |= progress.shards;
        combined.obelisks |= progress.obelisks;
        combined.legendItems |= progress.legendItems;
        // The native gate accepts each earlier stage from ANY party member.
        // Keep only the verified policy roster in this temporary union, so four
        // full save histories cannot overflow the bounded per-character record.
        for (const auto& stage : m_stageUnlocks.stages()) {
            if (progress.hasCompleted(stage.map)) {
                combined.recordCompleted(stage.map);
            }
        }
        for (const auto& stage : m_dreamcastPortalPolicy.stages()) {
            if (progress.hasCompleted(stage.map)) {
                combined.recordCompleted(stage.map);
            }
        }
    }
    // Native e00694e4 initializes an obelisk as spent if any active character
    // owns its bit. Later contacts award the whole party in one transaction.
    if (!m_progressInitialized) {
        for (auto& obelisk : m_obelisks) {
            auto& pickup = m_pickups[obelisk.pickup];
            if (pickup.award && combined.has(pickup.award->kind, pickup.award->index)) {
                pickup.collected = true;
            }
        }
    }
    m_partyProgress = std::move(combined);
    if (m_dreamcastTower) {
        m_dreamcastTower->synchronize(m_partyProgress);
        for (usize i = 0; i < m_dreamcastPortals.size(); ++i) {
            auto& portal = m_dreamcastPortalPresentation[i];
            const bool enabled =
                m_dreamcastPortalPolicy.allows(m_dreamcastPortals[i].map, m_partyProgress);
            if (portal.enabled != enabled) {
                portal.enabled = enabled;
                portal.motion.reset();
            }
            selectDreamcastPortal(i);
        }
    }
    if (m_platform == LegendsPlatform::N64 && m_gatedPortals) {
        for (usize i = 0; i < m_portals.size(); ++i) {
            auto& portal = m_portalPresentation[i];
            const bool enabled = m_stageUnlocks.allows(m_portals[i].map, m_partyProgress);
            if (portal.enabled != enabled) {
                portal.enabled = enabled;
                portal.motion.reset();
                selectPortal(i);
            }
        }
    }
    updateObelisks();
    updateDreamcastObelisks();
    updateTowerProgress(!m_progressInitialized);
    m_progressInitialized = true;
}

} // namespace gdl::game
