#include <algorithm>
#include <cmath>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

std::vector<LegendsContainerTarget> LegendsSceneAdapter::containerTargets() const {
    std::vector<LegendsContainerTarget> result;
    for (usize i = 0; i < m_containers.size(); ++i) {
        const auto& container = m_containers[i].state;
        if (container.state() != LegendsContainer::State::Closed) {
            continue;
        }
        const auto& definition = container.definition();
        const auto& box = definition.closedObstacle;
        result.push_back({static_cast<s32>(i), definition.position,
                          box.cylinderRadius > 0 ? box.cylinderRadius
                                                 : std::hypot(box.halfAcross, box.halfAlong),
                          box.height, container.targetable(), definition.nativeIndex,
                          definition.hazard != LegendsContainerDefinition::Hazard::None});
    }
    return result;
}

std::vector<Obstacle> LegendsSceneAdapter::containerObstacles() const {
    std::vector<Obstacle> result;
    for (const auto& obelisk : m_dreamcastObelisks)
        result.push_back(obelisk.body);
    for (const auto& obelisk : m_obelisks) {
        const auto& pickup = m_pickups[obelisk.pickup];
        Obstacle body;
        body.centre = pickup.position;
        body.cylinderRadius = pickup.radius;
        body.height = pickup.height;
        body.enemyItem = Obstacle::ItemQuery{true, pickup.radius};
        result.push_back(body);
    }
    for (const auto& gate : m_gates) {
        if (const auto barrier = gate.state.primaryObstacle(); barrier.solid) {
            result.push_back(barrier);
        }
        for (const auto& frame : gate.state.frameObstacles()) {
            if (frame.solid) {
                result.push_back(frame);
            }
        }
    }
    for (const auto& container : m_containers) {
        if (auto box = container.state.obstacle(); box.solid) {
            result.push_back(box);
        }
    }
    return result;
}

bool LegendsSceneAdapter::strikeContainer(s32 localId, f32 power, u32 flags, s32 byPlayer) {
    // The ordinary GDL attack resolver already computes damage and eligibility.
    // Native container flags are not numerically interchangeable with its flags.
    static_cast<void>(flags);
    if (localId < 0 || static_cast<usize>(localId) >= m_containers.size() ||
        !m_containers[static_cast<usize>(localId)].state.strike(power)) {
        return false;
    }
    if (m_containers[static_cast<usize>(localId)].state.state() != LegendsContainer::State::Opening)
        return true; // A stored-HP barrel accepted a blow but is not broken yet.
    openContainer(static_cast<usize>(localId));
    const auto& binding = m_containers[static_cast<usize>(localId)];
    const auto& definition = binding.state.definition();
    if (definition.hazard != LegendsContainerDefinition::Hazard::None) {
        const bool poison = definition.hazard == LegendsContainerDefinition::Hazard::Poison;
        LegendsFixtureEvent event;
        event.kind =
            poison ? LegendsFixtureEvent::Kind::Poison : LegendsFixtureEvent::Kind::Explosion;
        event.nativeIndex = definition.nativeIndex;
        event.position = definition.position + definition.blastOffset;
        event.radius = definition.blastRadius;
        event.damage = definition.blastDamage;
        event.flags = poison ? 0x260000U : 0x160001U;
        event.sound = definition.openingSound;
        event.player = byPlayer >= 0 && byPlayer < 4 ? byPlayer : -1;
        event.seconds = binding.blastSeconds;
        event.shrinkFraction = poison ? .1f : .2f;
        event.noFalloff = poison;
        m_fixtureEvents.push_back(event);
    }
    return true;
}

void LegendsSceneAdapter::openContainer(usize index) {
    selectDreamcastContainer(index);
    auto& container = m_containers[index];
    const auto& definition = container.state.definition();
    if (definition.openingSound != 0) {
        m_soundEvents.push_back({definition.openingSound, definition.position});
    }
    if (m_platform == LegendsPlatform::N64 &&
        definition.hazard == LegendsContainerDefinition::Hazard::Poison) {
        // e0074eec adds the gas cue independently of the broken-wood cue.
        m_soundEvents.push_back({60, definition.position + Vec3{0, 2, 0}});
    }
    if (container.selector) {
        m_lootSelectors.freeze(*container.selector);
        const auto choice = m_lootSelectors.selected(*container.selector);
        if (choice) {
            const auto outcome = std::ranges::find(container.outcomes, choice->resolvedTemplateKey,
                                                   &ContainerPresentation::Outcome::key);
            if (outcome != container.outcomes.end()) {
                container.openingFigure = outcome->openingFigure;
                container.death = outcome->death;
                container.pickup = outcome->pickup;
            }
        }
    }
    if (container.state.definition().broken.empty()) {
        container.brokenFigure = container.openingFigure;
    }
    m_hiddenFixtures[container.closedFigure] = true;
    m_hiddenFixtures[container.openingFigure] = false;
    m_fixtures[container.openingFigure].play(container.openingSequence, ItemFigure::Playback::Once);
    m_fixtures[container.openingFigure].capturePresentation();
    if (definition.hazard != LegendsContainerDefinition::Hazard::None) {
        // e00691bc spawns debris alongside the active tree, not after it.
        if (container.brokenFigure != container.openingFigure) {
            m_hiddenFixtures[container.brokenFigure] = false;
            m_fixtures[container.brokenFigure].play(0, ItemFigure::Playback::Once);
            m_fixtures[container.brokenFigure].capturePresentation();
        }
        if (container.blastFigure) {
            // The attack is born at this position. It is no longer a child of
            // the destroyed barrel's supporting platform on either console: DC
            // 8c0d6240 roots it under the effects tree 0x8c2b8750, while the
            // BAREXP0/BARPOI0 debris stays under the barrel's parent.
            const auto blast = [&](const NativeRider::Figure& bound) {
                return bound.index == *container.blastFigure;
            };
            for (auto& rider : m_nativeRiders) {
                std::erase_if(rider.figures, blast);
            }
            for (auto& rider : m_dreamcastRiders) {
                std::erase_if(rider.contents.figures, blast);
            }
            auto& figure = m_fixtures[*container.blastFigure];
            figure.placeAt(itemPlacement(definition.position + definition.blastOffset, Vec3{0}));
            figure.play(0, ItemFigure::Playback::Once);
            figure.capturePresentation();
            m_hiddenFixtures[*container.blastFigure] = false;
        }
    }
    if (container.pickup) {
        m_hiddenFixtures[m_pickupFigures[*container.pickup]] = false;
    }
}

void LegendsSceneAdapter::touchContainers(PlayerActor& actor) {
    for (usize i = 0; i < m_containers.size(); ++i) {
        auto& container = m_containers[i];
        auto& keys = actor.save().progress().inventory.keys;
        if (!container.state.touch(actor.position(), actor.reach(), actor.height(), keys)) {
            continue;
        }
        openContainer(i);
    }
}

void LegendsSceneAdapter::updateContainers() {
    for (auto& container : m_containers) {
        const auto& definition = container.state.definition();
        if (definition.hazard != LegendsContainerDefinition::Hazard::None) {
            if (container.state.state() != LegendsContainer::State::Closed) {
                for (const usize figure : {container.openingFigure, container.brokenFigure}) {
                    if (m_fixtures[figure].finished()) {
                        m_hiddenFixtures[figure] = true;
                    }
                }
                if (container.blastFigure && m_fixtures[*container.blastFigure].finished()) {
                    m_hiddenFixtures[*container.blastFigure] = true;
                }
                if (m_fixtures[container.openingFigure].finished()) {
                    container.state.finishOpening();
                }
            }
            continue;
        }
        if (container.state.state() == LegendsContainer::State::Opening) {
            auto& figure = m_fixtures[container.openingFigure];
            if (container.pickup && m_pickupFigures[*container.pickup] != container.openingFigure &&
                definition.kind == LegendsContainerDefinition::Kind::Chest) {
                // e006cab8 raises and scales chest contents with the native
                // opening frame, easing to one over at most 25 frames.
                const auto* sequence = figure.sequenceInfo(container.openingSequence);
                const f32 frames = sequence ? static_cast<f32>(sequence->frames) : 1;
                const f32 frame = figure.progress() * frames - 1;
                const f32 t = std::clamp(frame / std::clamp(frames - 1, 1.0f, 25.0f), 0.0f, 1.0f);
                const f32 scale = std::max(.2f, 1 - (1 - t) * (1 - t));
                const Vec3 position = definition.position + Vec3{0, scale, 0};
                m_fixtures[m_pickupFigures[*container.pickup]].placeAt(
                    glm::scale(itemPlacement(position, Vec3{0}), Vec3{scale}));
            }
            if (figure.finished() && container.state.finishOpening()) {
                if (container.brokenFigure != container.openingFigure ||
                    container.brokenSequence != container.openingSequence) {
                    m_hiddenFixtures[container.openingFigure] = true;
                    m_hiddenFixtures[container.brokenFigure] = false;
                    m_fixtures[container.brokenFigure].play(container.brokenSequence,
                                                            ItemFigure::Playback::Once);
                    m_fixtures[container.brokenFigure].capturePresentation();
                }
                if (container.pickup) {
                    m_pickups[*container.pickup].active = true;
                }
                if (container.death) {
                    // e006d570 releases Death directly when the container's
                    // opening completes; it is never an inventory offer.
                    auto& death = m_deaths[*container.death];
                    death.active = true;
                    death.state.placeAt(definition.position);
                    m_fixtures[death.figure].placeAt(itemPlacement(definition.position, Vec3{0}));
                    m_hiddenFixtures[death.figure] = false;
                    death.state.strike(1);
                }
            }
        }
        if (container.state.state() == LegendsContainer::State::Open && container.pickup &&
            m_pickups[*container.pickup].collected) {
            container.state.collected();
            if (definition.kind == LegendsContainerDefinition::Kind::Chest) {
                m_hiddenFixtures[container.openingFigure] = true;
                m_hiddenFixtures[container.brokenFigure] = true;
            }
        }
    }
}

} // namespace gdl::game
