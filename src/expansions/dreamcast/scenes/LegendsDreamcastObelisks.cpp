#include "expansions/dreamcast/level/LegendsDreamcastObelisks.h"

#include <algorithm>
#include <utility>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

bool LegendsSceneAdapter::bindDreamcastObelisk(const formats::ItemInfoRecord& info,
                                               const formats::ItemInstanceRecord& instance,
                                               u32 nativeIndex, usize figure) {
    std::string diagnostic;
    auto definition = legendsDreamcastObelisk(info, instance, nativeIndex, &diagnostic);
    const auto warn = [&](std::string message) {
        if (!message.empty() && std::ranges::find(m_warnings, message) == m_warnings.end()) {
            m_warnings.push_back(std::move(message));
        }
    };
    if (!definition || figure >= m_fixtures.size()) {
        warn(std::move(diagnostic));
        return false;
    }
    const auto* tree = m_fixtures[figure].tree();
    const auto off = tree ? tree->findSequence("OFF") : std::nullopt;
    const auto activating = tree ? tree->findSequence("ONA") : std::nullopt;
    const auto on = tree ? tree->findSequence("ON") : std::nullopt;
    if (!off || !activating || !on) {
        warn("Dreamcast obelisk is missing its OFF/ONA/ON artwork: " + definition->pickup.name);
        return false;
    }
    definition->pickup.position = m_fixtures[figure].position();
    definition->body.centre = definition->pickup.position;
    m_fixtures[figure].play(static_cast<s32>(*off), ItemFigure::Playback::Once);
    m_fixtures[figure].capturePresentation();
    m_dreamcastObelisks.push_back(
        {m_pickups.size(),
         figure,
         {static_cast<s32>(*off), static_cast<s32>(*activating), static_cast<s32>(*on)},
         definition->body,
         false});
    m_pickupFigures.push_back(figure);
    m_pickups.push_back(std::move(definition->pickup));
    return true;
}

void LegendsSceneAdapter::updateDreamcastObelisks() {
    for (auto& binding : m_dreamcastObelisks) {
        auto& pickup = m_pickups[binding.pickup];
        auto& figure = m_fixtures[binding.figure];
        binding.body.centre = pickup.position;
        if (!m_progressInitialized && pickup.award &&
            m_partyProgress.has(pickup.award->kind, pickup.award->index)) {
            pickup.collected = true;
        }
        if (!pickup.collected) {
            continue;
        }
        if (!binding.activated) {
            binding.activated = true;
            if (!m_progressInitialized) {
                // Rejoining an owned campaign does not replay the pickup cue.
                figure.play(binding.sequences[2], ItemFigure::Playback::Once);
                figure.capturePresentation();
            } else {
                // ProcessItems (8c0ab940) advances the native active tree. ONA's
                // authored 50 frames contain both the body swap and XPFF sprite
                // transforms; playing OFF forever had suppressed all of them.
                figure.play(binding.sequences[1], ItemFigure::Playback::Once);
                m_soundEvents.push_back({kDreamcastObeliskSound, pickup.position});
            }
        } else if (figure.sequence() == binding.sequences[1] && figure.finished()) {
            figure.play(binding.sequences[2], ItemFigure::Playback::Once);
        }
    }
}

} // namespace gdl::game
