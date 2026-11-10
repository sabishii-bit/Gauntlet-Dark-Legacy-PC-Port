#include <algorithm>
#include <format>

#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindDreamcastSwitch(RenderDevice& device, u32 nativeIndex) {
    if (!m_dreamcastMovement) {
        return;
    }
    const auto pads = m_dreamcastMovement->switches();
    const auto at =
        std::ranges::find(pads, nativeIndex, &LegendsDreamcastWorldMotion::Switch::nativeIndex);
    if (at == pads.end() || at->hidden) {
        return; // invisible contacts still live in the native movement adapter
    }
    ItemInstance placement;
    placement.position = at->position;
    placement.rotation = at->rotation;
    const usize figure = m_fixtures.size();
    place(device, at->name, placement, 1, object_flags::kSorted);
    if (figure == m_fixtures.size()) {
        m_warnings.push_back(
            std::format("Dreamcast switch {} artwork {} is unavailable", nativeIndex, at->name));
        return;
    }
    DreamcastSwitchPresentation binding;
    binding.index = static_cast<usize>(at - pads.begin());
    binding.figure = figure;
    constexpr std::array<std::string_view, 4> kNames{"OFF", "ONA", "ON", "OFFA"};
    const auto* tree = m_fixtures[figure].tree();
    if (tree) {
        for (usize i = 0; i < kNames.size(); ++i) {
            for (usize sequence = 0; sequence < tree->sequences.size(); ++sequence) {
                if (tree->sequences[sequence].name == kNames[i]) {
                    binding.sequences[i] = static_cast<s32>(sequence);
                }
            }
        }
    }
    if (binding.sequences[0] >= 0) {
        m_fixtures[figure].play(binding.sequences[0], ItemFigure::Playback::Loop);
    }
    m_dreamcastSwitches.push_back(binding);
}

void LegendsSceneAdapter::updateDreamcastSwitches() {
    if (!m_dreamcastMovement) {
        return;
    }
    const auto movementSounds = m_dreamcastMovement->takeSounds();
    m_soundEvents.insert(m_soundEvents.end(), movementSounds.begin(), movementSounds.end());
    for (auto& binding : m_dreamcastSwitches) {
        const auto& pad = m_dreamcastMovement->switches()[binding.index];
        auto& figure = m_fixtures[binding.figure];
        figure.placeAt(m_dreamcastMovement->riderTransform(pad.floor) *
                       legendsDreamcastItemPlacement(pad.position, pad.rotation));
        const s32 transition = binding.sequences[pad.active ? 1 : 3];
        const s32 idle = binding.sequences[pad.active ? 2 : 0];
        if (binding.active == pad.active) {
            if (figure.finished() && figure.sequence() == transition && idle >= 0) {
                figure.play(idle, ItemFigure::Playback::Loop);
            }
            continue;
        }
        binding.active = pad.active;
        if (transition >= 0) {
            figure.play(transition, ItemFigure::Playback::Once);
        } else if (idle >= 0) {
            figure.play(idle, ItemFigure::Playback::Loop);
        }
    }
}

void LegendsSceneAdapter::touchDreamcastSwitches(std::span<const LegendsPortalVisitor> visitors) {
    m_dreamcastMovement->touch(visitors);
    touchDreamcastFalling(visitors);
    updateDreamcastSwitches();
}

} // namespace gdl::game
