#include <algorithm>
#include <cmath>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindN64Generator(
    RenderDevice& device, const formats::LegendsN64Fixture& fixture, std::span<const u8> templates,
    std::string_view directory, std::span<const formats::LegendsMapCollisionTriangle> collision,
    usize idleFigure) {
    if (idleFigure >= m_fixtures.size()) {
        return;
    }
    const auto warn = [&](const std::string& text) {
        if (!text.empty() && std::ranges::find(m_warnings, text) == m_warnings.end()) {
            m_warnings.push_back(text);
        }
    };
    std::string diagnostic;
    auto definition = legendsN64Generator(fixture, templates, directory, collision, &diagnostic);
    warn(diagnostic);
    if (!definition) {
        return;
    }
    AssetScope scope;
    for (auto& archive : m_archives) {
        scope.add(*archive);
    }
    // An unsupported damage state must not turn a live generator invisible.
    // Admission requires every authored state, including an optional ruin.
    for (const auto& name : definition->sequences) {
        if (!name.empty() && !scope.findTree(name)) {
            warn("Native generator damage artwork is unavailable: " + name);
            return;
        }
    }
    GeneratorPresentation binding;
    binding.nativeIndex = definition->gameplay.nativeIndex;
    binding.originalFigure = idleFigure;
    binding.yaw = definition->gameplay.yaw;
    ItemInstance placement;
    placement.position = definition->gameplay.position;
    placement.rotation.y = binding.yaw;
    for (usize state = 0; state < definition->sequences.size(); ++state) {
        const auto& name = definition->sequences[state];
        if (name.empty()) {
            continue;
        }
        if (state == static_cast<usize>(definition->gameplay.tier) &&
            m_fixtures[idleFigure].tree() != nullptr &&
            name == m_fixtures[idleFigure].tree()->name) {
            binding.figures[state] = idleFigure;
            continue;
        }
        const usize slot = m_fixtures.size();
        place(device, name, placement);
        if (m_fixtures.size() == slot) {
            warn("Native generator damage artwork could not bind: " + name);
            return;
        }
        binding.figures[state] = slot;
        m_hiddenFixtures[slot] = true;
        const auto* sequence = m_fixtures[slot].sequenceInfo(0);
        m_fixtures[slot].play(0, sequence != nullptr && sequence->repeats
                                     ? ItemFigure::Playback::Loop
                                     : ItemFigure::Playback::Once);
    }
    // Keep the resolved initial figure until the GDL pool confirms admission.
    m_generators.push_back(std::move(definition->gameplay));
    m_generatorPresentation.push_back(binding);
}

void LegendsSceneAdapter::updateGenerators(std::span<const ImportedGeneratorState> states) {
    for (const auto& state : states) {
        if (state.pool < 0 || state.state < 0 || state.state > 3 ||
            !std::isfinite(state.position.x) || !std::isfinite(state.position.y) ||
            !std::isfinite(state.position.z)) {
            continue;
        }
        const auto found = std::ranges::find(m_generatorPresentation, state.nativeIndex,
                                             &GeneratorPresentation::nativeIndex);
        if (found == m_generatorPresentation.end()) {
            continue;
        }
        auto& binding = *found;
        const auto figure = state.standing || state.state == 0
                                ? binding.figures[static_cast<usize>(state.state)]
                                : std::nullopt;
        if (!binding.installed) {
            m_hiddenFixtures[binding.originalFigure] = true;
            binding.installed = true;
        }
        if (binding.active != figure) {
            if (binding.active) {
                m_hiddenFixtures[*binding.active] = true;
            }
            binding.active = figure;
            if (figure) {
                m_hiddenFixtures[*figure] = false;
                const auto* sequence = m_fixtures[*figure].sequenceInfo(0);
                m_fixtures[*figure].play(0, sequence != nullptr && sequence->repeats
                                                ? ItemFigure::Playback::Loop
                                                : ItemFigure::Playback::Once);
                m_fixtures[*figure].capturePresentation();
            }
        }
        if (figure) {
            m_fixtures[*figure].placeAt(itemPlacement(state.position, Vec3{0, binding.yaw, 0}));
        }
    }
}

} // namespace gdl::game
