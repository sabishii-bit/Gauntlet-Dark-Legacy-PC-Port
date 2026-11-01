#include <algorithm>
#include <cmath>
#include <utility>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindN64Trap(RenderDevice& device, formats::LegendsN64Trap definition,
                                      usize idleFigure, f32 damageScale, s32 floorOwner) {
    if (idleFigure >= m_fixtures.size()) {
        return;
    }
    definition.position = m_fixtures[idleFigure].position();
    AssetScope scope;
    for (auto& archive : m_archives) {
        scope.add(*archive);
    }
    ItemInstance placement;
    placement.position = definition.position;
    placement.rotation.y = definition.yaw;
    const auto variant = [&](const std::string& name) -> std::optional<usize> {
        if (name.empty() || !scope.findTree(formats::legendsN64TreeName("TRAP", name))) {
            return {};
        }
        const usize at = m_fixtures.size();
        place(device, formats::legendsN64TreeName("TRAP", name), placement, definition.scale);
        if (at == m_fixtures.size()) {
            return {};
        }
        m_hiddenFixtures[at] = true;
        m_heldFixtures[at] = true; // This native state machine owns its animation clock.
        return at;
    };
    const auto active = variant(definition.activeSequence);
    if (!active) {
        m_warnings.push_back("Native damaging trap active artwork is unavailable: " +
                             definition.activeSequence);
        return;
    }
    std::optional<usize> starting;
    std::optional<usize> ending;
    if (definition.activeSequence.back() == 'A') {
        auto name = definition.activeSequence;
        name.back() = 'S';
        starting = variant(name);
        name = definition.idleSequence;
        name.back() = 'E';
        ending = variant(name);
    }
    const auto frames = [&](usize index) -> u32 {
        const auto* animation = m_fixtures[index].sequenceInfo(0);
        return animation && animation->frames > 0 ? static_cast<u32>(animation->frames) : 0;
    };
    if (frames(*active) == 0 || (starting && frames(*starting) == 0)) {
        m_warnings.emplace_back("Native damaging trap has no valid authored duration.");
        return;
    }
    if (!definition.contactNode.empty() &&
        (!m_fixtures[*active].nodeTransform(definition.contactNode) ||
         (starting && !m_fixtures[*starting].nodeTransform(definition.contactNode)))) {
        m_warnings.push_back("Native damaging trap attachment is unavailable: " +
                             definition.contactNode);
        return;
    }
    const bool hidden = (definition.placementFlags & 8U) != 0;
    m_hiddenFixtures[idleFigure] = hidden;
    m_heldFixtures[idleFigure] = true;
    const Vec3 basePosition = definition.position;
    LegendsTrap state{std::move(definition), damageScale, frames(*active),
                      starting ? frames(*starting) : 0, ending.has_value()};
    m_traps.push_back(
        {std::move(state), {idleFigure, starting, active, ending}, 0, floorOwner, basePosition});
}

void LegendsSceneAdapter::setTrapView(const ViewVolume& view) {
    m_trapView = view;
}

void LegendsSceneAdapter::updateTraps(f32 seconds, bool timeStopped) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 10) {
        return;
    }
    for (auto& binding : m_traps) {
        auto& state = binding.state;
        const Vec3 position =
            binding.basePosition + m_worldMovement.supportingOffset(binding.floorOwner);
        state.placeAt(position);
        const auto& native = state.definition();
        const auto placement =
            glm::scale(itemPlacement(position, Vec3{0, native.yaw, 0}), Vec3{native.scale});
        for (const auto index : binding.figures) {
            if (index) {
                m_fixtures[*index].placeAt(placement);
            }
        }
        const auto current = binding.figures[static_cast<usize>(state.phase())];
        if (!current) {
            continue;
        }
        auto& oldFigure = m_fixtures[*current];
        // Stop Time gates e0068118's trap state, not the independently ticking
        // tree animation in e0032c80/e00327e4. Existing attacks still age too.
        oldFigure.update(seconds);
        const auto body = state.contact();
        const Vec3 centre = body.centre + Vec3{0, body.height * .5f, 0};
        const f32 radius = glm::length(Vec3{body.halfAcross, body.height * .5f, body.halfAlong});
        const bool visible = !m_trapView || m_trapView->sees(centre, radius);
        state.advance(seconds, visible, timeStopped, oldFigure.finished());
        if (state.generation() != binding.generation) {
            binding.generation = state.generation();
            const auto selected = binding.figures[static_cast<usize>(state.phase())];
            for (const auto index : binding.figures) {
                if (index) {
                    m_hiddenFixtures[*index] =
                        index != selected || (native.placementFlags & 8U) != 0;
                }
            }
            if (selected) {
                auto& figure = m_fixtures[*selected];
                const auto* animation = figure.sequenceInfo(0);
                figure.play(0, state.phase() == LegendsTrap::Phase::Idle && animation != nullptr &&
                                   animation->repeats);
                figure.capturePresentation();
            }
        }
        if (!native.contactNode.empty()) {
            const auto selected = binding.figures[static_cast<usize>(state.phase())];
            const auto node =
                selected ? m_fixtures[*selected].nodeTransform(native.contactNode) : std::nullopt;
            state.nodeContact(node ? std::optional<Vec3>{Vec3{(*node)[3]}} : std::nullopt);
        }
    }
}

void LegendsSceneAdapter::touchTraps(std::span<const LegendsPortalVisitor> visitors) {
    for (auto& binding : m_traps) {
        for (const auto& visitor : visitors) {
            if (auto hit = binding.state.touch(visitor)) {
                m_trapHits.push_back(*hit);
            }
        }
    }
}

std::vector<LegendsTrapHit> LegendsSceneAdapter::takeTrapHits() {
    return std::exchange(m_trapHits, {});
}
} // namespace gdl::game
