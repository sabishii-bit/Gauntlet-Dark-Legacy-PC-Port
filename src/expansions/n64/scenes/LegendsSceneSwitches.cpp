#include <algorithm>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindN64Switch(RenderDevice& device,
                                        const formats::LegendsN64Switch& definition,
                                        usize idleFigure) {
    if (!definition.supported || idleFigure >= m_fixtures.size()) {
        return;
    }
    SwitchPresentation binding{definition.nativeIndex, idleFigure};
    binding.yaw = definition.yaw;
    binding.scale = definition.scale;
    // e0068118 suppresses the plate cue for invisible activation volumes.
    binding.sound = definition.hidden() ? 0 : definition.sound;
    m_hiddenFixtures[idleFigure] = definition.hidden();
    if (!definition.activeSequence.empty()) {
        ItemInstance placement;
        placement.position = m_fixtures[idleFigure].position();
        placement.rotation.y = definition.yaw;
        const usize active = m_fixtures.size();
        place(device, formats::legendsN64TreeName("TRAP", definition.activeSequence), placement,
              definition.scale);
        if (m_fixtures.size() != active) {
            binding.activeFigure = active;
            m_hiddenFixtures[active] = true;
        } else {
            m_warnings.push_back("Native switch active artwork is unavailable: " +
                                 definition.activeSequence);
        }
    }
    // Hidden contact volumes have no plate to reveal when activated.
    if (definition.hidden()) {
        binding.activeFigure.reset();
    }
    m_switchPresentation.push_back(binding);
}

void LegendsSceneAdapter::bindWorldMotion(const formats::LegendsMap& map,
                                          std::span<const formats::LegendsN64Switch> definitions) {
    m_worldMovement = LegendsWorldMotion{map, definitions, static_cast<u8>(m_playerCount)};
    m_warnings.insert(m_warnings.end(), m_worldMovement.warnings().begin(),
                      m_worldMovement.warnings().end());
    updateSwitches();
}

void LegendsSceneAdapter::touchSwitches(std::span<const LegendsPortalVisitor> visitors) {
    m_worldMovement.touch(visitors);
    updateSwitches();
}

void LegendsSceneAdapter::updateSwitches() {
    const auto movement = m_worldMovement.takeSounds();
    m_soundEvents.insert(m_soundEvents.end(), movement.begin(), movement.end());
    for (auto& plate : m_switchPresentation) {
        if (const auto position = m_worldMovement.platePosition(plate.nativeIndex)) {
            const auto placement =
                glm::scale(itemPlacement(*position, Vec3{0, plate.yaw, 0}), Vec3{plate.scale});
            m_fixtures[plate.idleFigure].placeAt(placement);
            if (plate.activeFigure) {
                m_fixtures[*plate.activeFigure].placeAt(placement);
            }
        }
        const bool active = m_worldMovement.activated(plate.nativeIndex);
        if (plate.activated == active) {
            continue;
        }
        plate.activated = active;
        if (active && plate.sound != 0) {
            m_soundEvents.push_back({plate.sound, m_fixtures[plate.idleFigure].position()});
        }
        if (plate.activeFigure) {
            m_hiddenFixtures[plate.idleFigure] = active;
            m_hiddenFixtures[*plate.activeFigure] = !active;
            auto& figure = m_fixtures[active ? *plate.activeFigure : plate.idleFigure];
            const auto* sequence = figure.sequenceInfo(0);
            figure.play(0, sequence != nullptr && sequence->repeats ? ItemFigure::Playback::Loop
                                                                    : ItemFigure::Playback::Once);
            figure.capturePresentation();
        }
    }
}

} // namespace gdl::game
