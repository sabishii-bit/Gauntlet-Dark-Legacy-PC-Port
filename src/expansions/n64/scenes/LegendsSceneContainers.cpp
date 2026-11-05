#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Error.h"

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindN64Selectors(std::span<const u8> powerups,
                                           std::span<const u8> randoms,
                                           std::span<const formats::LegendsN64Fixture> fixtures) {
    m_lootSelectors = LegendsLootSelectors{powerups, randoms, fixtures, m_playerCount};
}

void LegendsSceneAdapter::bindN64FloorSelector(RenderDevice& device,
                                               const formats::LegendsN64Fixture& fixture,
                                               usize idleFigure) {
    if (fixture.kind != formats::LegendsN64FixtureKind::Powerup ||
        idleFigure >= m_fixtures.size()) {
        return;
    }
    const auto slot = m_lootSelectors.find(fixture.nativeIndex, false);
    if (!slot) {
        return;
    }
    FloorSelectorPresentation binding;
    binding.slot = *slot;
    binding.selected = 0xffff;
    binding.activeFigure = idleFigure;
    const auto old = std::ranges::find(m_pickupFigures, idleFigure);
    if (old != m_pickupFigures.end()) {
        binding.pickup = static_cast<usize>(old - m_pickupFigures.begin());
    } else {
        binding.pickup = m_pickups.size();
        LegendsPickup placeholder;
        placeholder.position = m_fixtures[idleFigure].position();
        placeholder.active = false;
        m_pickups.push_back(std::move(placeholder));
        m_pickupFigures.push_back(idleFigure);
    }
    const Vec3 position = m_fixtures[idleFigure].position();
    for (auto choice : m_lootSelectors.outcomes(*slot)) {
        choice.position = position;
        FloorSelectorPresentation::Outcome outcome;
        outcome.key = choice.resolvedTemplateKey;
        if (outcome.key == fixture.resolvedTemplateKey) {
            outcome.figure = idleFigure;
        } else if (!choice.sequence.empty()) {
            const usize index = m_fixtures.size();
            ItemInstance placement;
            placement.position = position;
            place(device, formats::legendsN64TreeName("POWERUP", choice.sequence), placement);
            if (index != m_fixtures.size()) {
                outcome.figure = index;
            }
        }
        std::string diagnostic;
        if (outcome.figure) {
            m_hiddenFixtures[*outcome.figure] = true;
            outcome.death = bindN64Death(choice, *outcome.figure, {});
            if (outcome.death) {
                m_deaths[*outcome.death].active = false;
            } else {
                outcome.pickup = legendsN64Pickup(choice, &diagnostic, m_playerCount);
            }
        }
        if (!diagnostic.empty() && std::ranges::find(m_warnings, diagnostic) == m_warnings.end()) {
            m_warnings.push_back(std::move(diagnostic));
        }
        binding.outcomes.push_back(std::move(outcome));
    }
    m_floorSelectors.push_back(std::move(binding));
    selectFloorOutcome(m_floorSelectors.size() - 1);
}

void LegendsSceneAdapter::selectFloorOutcome(usize index) {
    auto& binding = m_floorSelectors[index];
    const auto choice = m_lootSelectors.selected(binding.slot);
    const u16 selected = choice ? choice->resolvedTemplateKey : 0xffff;
    if (binding.selected == selected) {
        return;
    }
    if (binding.activeFigure) {
        m_hiddenFixtures[*binding.activeFigure] = true;
    }
    binding.selected = selected;
    binding.activeFigure.reset();
    for (const auto& old : binding.outcomes) {
        if (old.death)
            m_deaths[*old.death].active = false;
    }
    auto& pickup = m_pickups[binding.pickup];
    pickup.active = false;
    const auto outcome =
        std::ranges::find(binding.outcomes, selected, &FloorSelectorPresentation::Outcome::key);
    if (outcome == binding.outcomes.end()) {
        return;
    }
    binding.activeFigure = outcome->figure;
    if (outcome->death)
        m_deaths[*outcome->death].active = true;
    if (outcome->figure) {
        auto& figure = m_fixtures[*outcome->figure];
        const auto* animation = figure.sequenceInfo(0);
        figure.play(0, animation != nullptr && animation->repeats ? ItemFigure::Playback::Loop
                                                                  : ItemFigure::Playback::Once);
        figure.capturePresentation();
        m_hiddenFixtures[*outcome->figure] = false;
        m_pickupFigures[binding.pickup] = *outcome->figure;
    }
    if (outcome->pickup) {
        pickup = *outcome->pickup;
    }
}

void LegendsSceneAdapter::updateContainerSelectors(f32 seconds) {
    for (auto& binding : m_floorSelectors) {
        if (m_pickups[binding.pickup].collected) {
            m_lootSelectors.disable(binding.slot);
        }
    }
    m_lootSelectors.advance(seconds);
    for (usize i = 0; i < m_floorSelectors.size(); ++i) {
        if (m_lootSelectors.slots()[m_floorSelectors[i].slot].active) {
            selectFloorOutcome(i);
        }
    }
}

void LegendsSceneAdapter::prepareSelectorPickup(usize pickup) {
    for (usize i = 0; i < m_floorSelectors.size(); ++i) {
        auto& binding = m_floorSelectors[i];
        if (binding.pickup != pickup || m_pickups[pickup].collected) {
            continue;
        }
        m_lootSelectors.query(binding.slot);
        selectFloorOutcome(i);
        break;
    }
}

void LegendsSceneAdapter::bindN64Container(RenderDevice& device,
                                           LegendsContainerDefinition definition,
                                           usize idleFigure) {
    if (idleFigure >= m_fixtures.size()) {
        return;
    }
    if (m_containers.size() >= static_cast<usize>(LegendsDeath::kTargetBase)) {
        throw FormatError("Too many native container combat targets");
    }
    const auto warn = [&](const std::string& warning) {
        if (!warning.empty() && std::ranges::find(m_warnings, warning) == m_warnings.end()) {
            m_warnings.push_back(warning);
        }
    };
    warn(definition.diagnostic);
    AssetScope scope;
    for (auto& archive : m_archives) {
        scope.add(*archive);
    }
    const auto tree = [&](std::string_view name) {
        return formats::legendsN64TreeName(definition.group, name);
    };
    if (definition.opening.empty() || !scope.findTree(tree(definition.opening))) {
        warn("Native container opening artwork is unavailable: " + definition.opening);
        return;
    }
    // Floor projection was resolved by the native placement reader before the
    // initial figure was placed. Reuse that exact position for body and loot.
    definition.position = m_fixtures[idleFigure].position();
    definition.closedObstacle.centre = definition.position;
    if (definition.contents) {
        definition.contents->position = definition.position;
    }
    ContainerPresentation binding{LegendsContainer{definition},
                                  idleFigure,
                                  idleFigure,
                                  idleFigure,
                                  std::nullopt,
                                  std::nullopt,
                                  {},
                                  std::nullopt,
                                  0};
    std::vector<formats::LegendsN64Fixture> choices;
    if (definition.selector) {
        binding.selector = m_lootSelectors.find(definition.nativeIndex, true);
        if (!binding.selector) {
            warn("Native container selector state is unavailable.");
        } else {
            choices = m_lootSelectors.outcomes(*binding.selector);
        }
    } else if (definition.contents) {
        choices.push_back(*definition.contents);
    }
    ItemInstance instance;
    instance.position = definition.position;
    instance.rotation.y = -definition.yaw;
    const auto variant = [&](std::string_view name, usize fallback) {
        if (name.empty() || !scope.findTree(tree(name))) {
            return fallback;
        }
        const usize index = m_fixtures.size();
        place(device, tree(name), instance, definition.scale);
        if (m_fixtures.size() == index) {
            return fallback;
        }
        m_hiddenFixtures[index] = true;
        m_fixtures[index].play(0, ItemFigure::Playback::Once);
        return index;
    };
    binding.openingFigure = variant(definition.opening, idleFigure);
    if (binding.openingFigure == idleFigure) {
        return;
    }
    binding.brokenFigure = variant(definition.broken, binding.openingFigure);
    if (definition.hazard != LegendsContainerDefinition::Hazard::None) {
        const bool poison = definition.hazard == LegendsContainerDefinition::Hazard::Poison;
        const auto blastTree = formats::legendsN64TreeName("WEAPONS", poison ? "PBAL" : "FBAL");
        const usize index = m_fixtures.size();
        ItemInstance blastPlacement;
        blastPlacement.position = definition.position + Vec3{0, 2, 0};
        place(device, blastTree, blastPlacement);
        if (index == m_fixtures.size()) {
            warn("Native explosive-barrel effect is unavailable: " + blastTree);
            return;
        }
        binding.blastFigure = index;
        m_hiddenFixtures[index] = true;
        m_fixtures[index].play(0, ItemFigure::Playback::Once);
        const auto* sequence = m_fixtures[index].sequenceInfo(0);
        if (!sequence || sequence->frames <= 0) {
            warn("Native explosive-barrel effect has no duration: " + blastTree);
            return;
        }
        binding.blastSeconds =
            static_cast<f32>(sequence->frames) *
            static_cast<f32>(sequence->frameRate > 0 ? sequence->frameRate : 30) / 900.0f;
    }
    usize goldOpening = binding.openingFigure;
    if (std::ranges::any_of(
            choices, [](const auto& choice) { return (choice.resolvedTemplateKey >> 8U) == 3; })) {
        std::string gold = definition.opening;
        gold.back() = 'G';
        goldOpening = variant(gold, binding.openingFigure);
    }
    for (auto choice : choices) {
        choice.position = definition.position;
        ContainerPresentation::Outcome outcome;
        outcome.key = choice.resolvedTemplateKey;
        const bool goldFigure = (outcome.key >> 8U) == 3 && goldOpening != binding.openingFigure;
        outcome.openingFigure = goldFigure ? goldOpening : binding.openingFigure;
        std::string diagnostic;
        if (LegendsDeath::decode(choice, m_playerCount)) {
            const usize figure = m_fixtures.size();
            ItemInstance placement;
            placement.position = definition.position;
            place(device, formats::legendsN64TreeName("POWERUP", choice.sequence), placement);
            if (m_fixtures.size() != figure) {
                outcome.death = bindN64Death(choice, figure, {});
                if (outcome.death)
                    m_deaths[*outcome.death].active = false;
                m_hiddenFixtures[figure] = true;
            } else {
                warn("Native contained Death artwork is unavailable");
            }
            binding.outcomes.push_back(outcome);
            continue;
        }
        auto pickup = legendsN64Pickup(choice, &diagnostic, m_playerCount);
        warn(diagnostic);
        if (pickup) {
            usize figure = outcome.openingFigure;
            if (!goldFigure) {
                const usize index = m_fixtures.size();
                ItemInstance contentPlacement;
                contentPlacement.position = definition.position;
                place(device, formats::legendsN64TreeName("POWERUP", choice.sequence),
                      contentPlacement);
                if (m_fixtures.size() != index) {
                    figure = index;
                    m_hiddenFixtures[figure] = true;
                    const auto* animation = m_fixtures[figure].sequenceInfo(0);
                    m_fixtures[figure].play(0, animation != nullptr && animation->repeats
                                                   ? ItemFigure::Playback::Loop
                                                   : ItemFigure::Playback::Once);
                } else {
                    // Retain no invisible collectible when its native figure is absent.
                    pickup.reset();
                }
            }
            if (pickup) {
                pickup->active = false;
                // A contained item is reached through the container's volume,
                // not the tiny cylinder of a separate floor pickup.
                const auto& box = definition.closedObstacle;
                pickup->position = definition.position;
                pickup->radius = box.cylinderRadius > 0 ? box.cylinderRadius
                                                        : std::hypot(box.halfAcross, box.halfAlong);
                pickup->height = box.height;
                if (box.cylinderRadius == 0) {
                    pickup->collision = LegendsPickupCollision::Box;
                    pickup->xSize = box.halfAcross;
                    pickup->zSize = box.halfAlong;
                    pickup->yaw = box.yaw;
                }
                outcome.pickup = m_pickups.size();
                m_pickups.push_back(std::move(*pickup));
                m_pickupFigures.push_back(figure);
            }
        }
        binding.outcomes.push_back(outcome);
    }
    if (!binding.selector && !binding.outcomes.empty()) {
        binding.openingFigure = binding.outcomes.front().openingFigure;
        binding.pickup = binding.outcomes.front().pickup;
        binding.death = binding.outcomes.front().death;
    }
    if (definition.broken.empty()) {
        binding.brokenFigure = binding.openingFigure;
    }
    m_containers.push_back(std::move(binding));
}

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
    openContainer(static_cast<usize>(localId));
    const auto& binding = m_containers[static_cast<usize>(localId)];
    const auto& definition = binding.state.definition();
    if (definition.hazard != LegendsContainerDefinition::Hazard::None) {
        const bool poison = definition.hazard == LegendsContainerDefinition::Hazard::Poison;
        LegendsFixtureEvent event;
        event.kind =
            poison ? LegendsFixtureEvent::Kind::Poison : LegendsFixtureEvent::Kind::Explosion;
        event.nativeIndex = definition.nativeIndex;
        event.position = definition.position + Vec3{0, 2, 0};
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
    auto& container = m_containers[index];
    const auto& definition = container.state.definition();
    if (definition.openingSound != 0) {
        m_soundEvents.push_back({definition.openingSound, definition.position});
    }
    if (definition.hazard == LegendsContainerDefinition::Hazard::Poison) {
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
    m_fixtures[container.openingFigure].play(0, ItemFigure::Playback::Once);
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
            // the destroyed barrel's supporting platform.
            for (auto& rider : m_nativeRiders) {
                std::erase_if(rider.figures, [&](const auto& bound) {
                    return bound.index == *container.blastFigure;
                });
            }
            auto& figure = m_fixtures[*container.blastFigure];
            figure.placeAt(itemPlacement(definition.position + Vec3{0, 2, 0}, Vec3{0}));
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
                const auto* sequence = figure.sequenceInfo(0);
                const f32 frames = sequence ? static_cast<f32>(sequence->frames) : 1;
                const f32 frame = figure.progress() * frames - 1;
                const f32 t = std::clamp(frame / std::clamp(frames - 1, 1.0f, 25.0f), 0.0f, 1.0f);
                const f32 scale = std::max(.2f, 1 - (1 - t) * (1 - t));
                const Vec3 position = definition.position + Vec3{0, scale, 0};
                m_fixtures[m_pickupFigures[*container.pickup]].placeAt(
                    glm::scale(itemPlacement(position, Vec3{0}), Vec3{scale}));
            }
            if (figure.finished() && container.state.finishOpening()) {
                if (container.brokenFigure != container.openingFigure) {
                    m_hiddenFixtures[container.openingFigure] = true;
                    m_hiddenFixtures[container.brokenFigure] = false;
                    m_fixtures[container.brokenFigure].play(0, ItemFigure::Playback::Once);
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
