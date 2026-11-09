#include <algorithm>
#include <cmath>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindDreamcastTrap(const formats::ItemInfoRecord& info,
                                            const formats::ItemInstanceRecord& instance,
                                            u32 nativeIndex, usize figureIndex) {
    if (figureIndex >= m_fixtures.size())
        return;
    const auto warn = [&](std::string message) {
        if (std::ranges::find(m_warnings, message) == m_warnings.end())
            m_warnings.push_back(std::move(message));
    };
    auto& figure = m_fixtures[figureIndex];
    const auto* tree = figure.tree();
    if (!tree) {
        warn("Dreamcast trap has no native animation tree: " + info.name);
        return;
    }
    std::array<s32, 4> sequences{};
    std::array<s32, 4> ticks{};
    constexpr std::array<std::string_view, 4> names{"OFF", "ONA", "ON", "ONB"};
    for (usize i = 0; i < names.size(); ++i) {
        const auto found = tree->findSequence(names[i]);
        if (!found) {
            warn("Dreamcast trap is missing native sequence " + std::string{names[i]} + ": " +
                 info.name);
            return;
        }
        sequences[i] = static_cast<s32>(*found);
        ticks[i] = figure.ticksOf(sequences[i]);
    }
    std::string diagnostic;
    auto trap = LegendsDreamcastTrap::decode(info, instance, nativeIndex, ticks, &diagnostic);
    if (!diagnostic.empty())
        warn(diagnostic + ": " + info.name);
    if (!trap)
        return;
    figure.play(sequences[0], ItemFigure::Playback::Loop);
    m_heldFixtures[figureIndex] = true;
    m_dreamcastTraps.push_back({std::move(*trap), figureIndex, sequences, 0});
}

void LegendsSceneAdapter::updateDreamcastTraps(f32 seconds, bool timeStopped) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 10)
        return;
    for (auto& cooldown : m_dreamcastTrapCooldowns)
        cooldown = std::max(0.f, cooldown - seconds);
    for (auto& binding : m_dreamcastTraps) {
        auto& figure = m_fixtures[binding.figure];
        binding.state.advance(seconds, timeStopped);
        const auto sequence = binding.sequences[static_cast<usize>(binding.state.action())];
        // Stop Time clears the native actions, not the animation clock.
        // AnimateATree mode 0 finishes the current sequence before OFF;
        // switching to an empty OFF early would hold half-raised spikes.
        if (binding.state.action() == 0 && figure.sequence() != sequence && !figure.finished()) {
            figure.update(seconds);
            continue;
        }
        if (binding.generation != binding.state.generation()) {
            binding.generation = binding.state.generation();
            figure.play(sequence, binding.state.action() == 0 ? ItemFigure::Playback::Loop
                                                              : ItemFigure::Playback::Once);
            figure.capturePresentation();
        } else {
            figure.update(seconds);
        }
    }
}

void LegendsSceneAdapter::touchDreamcastTraps(std::span<const LegendsPortalVisitor> visitors) {
    for (auto& binding : m_dreamcastTraps)
        for (const auto& visitor : visitors) {
            if (visitor.player >= m_dreamcastTrapCooldowns.size() ||
                m_dreamcastTrapCooldowns[visitor.player] > 0)
                continue;
            if (auto hit = binding.state.touch(visitor)) {
                // ItemTouch's fxhittime belongs to the player, not the trap.
                // Adjacent spikes must not each deal damage in the same strike.
                m_dreamcastTrapCooldowns[visitor.player] =
                    static_cast<f32>(binding.state.ticksLeft() + 1) / 30;
                m_trapHits.push_back(*hit);
                // AudioTrap is a contact cue, not an animation-start cue.
                if (const u32 sound = binding.state.contactSound(m_dreamcastRealm); sound != 0)
                    m_soundEvents.push_back({sound, visitor.position});
            }
        }
}

} // namespace gdl::game
