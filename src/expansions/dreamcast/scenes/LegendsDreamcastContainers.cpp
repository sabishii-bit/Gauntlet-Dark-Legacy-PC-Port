#include "expansions/dreamcast/level/LegendsDreamcastContainers.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindDreamcastContainer(RenderDevice& device,
                                                 const formats::ItemInfoRecord& info,
                                                 const formats::ItemInstanceRecord& instance,
                                                 u32 nativeIndex, usize idleFigure, s32 realmType) {
    if (idleFigure >= m_fixtures.size())
        return;
    const auto warn = [&](const std::string& text) {
        if (!text.empty() && std::ranges::find(m_warnings, text) == m_warnings.end())
            m_warnings.push_back(text);
    };
    std::string diagnostic;
    auto definition = legendsDreamcastContainer(info, instance, nativeIndex, &diagnostic);
    warn(diagnostic);
    if (!definition)
        return;
    if (m_containers.size() >= kMostContainers)
        throw FormatError("Too many native container combat targets");
    const auto sequence = [&](usize figure, std::string_view name) -> s32 {
        const auto* tree = m_fixtures[figure].tree();
        const auto found = tree ? tree->findSequence(name) : std::nullopt;
        return found ? static_cast<s32>(*found) : -1;
    };
    definition->openingSound = dreamcastContainerSound(realmType, info.subtype);
    const s32 idle = sequence(idleFigure, info.subtype <= 45 ? "IDLE" : "CLOSED");
    const s32 opening = sequence(idleFigure, definition->opening);
    const s32 broken = sequence(idleFigure, definition->broken);
    if (idle < 0 || opening < 0 || broken < 0 || m_fixtures[idleFigure].ticksOf(opening) <= 0) {
        definition->interactive = false;
        warn("Dreamcast container is missing native state sequences: " + definition->closed);
    }
    if (idle >= 0)
        m_fixtures[idleFigure].play(idle, ItemFigure::Playback::Loop);
    // Type 10's first parameter is its trap subtype, not a loot definition.
    const s16 contents = info.type == 2 ? std::bit_cast<s16>(readU16LE(instance.params, 0)) : -1;
    ContainerPresentation binding{LegendsContainer{*definition}};
    binding.closedFigure = binding.openingFigure = binding.brokenFigure = idleFigure;
    binding.openingSequence = opening;
    binding.brokenSequence = broken;
    binding.dreamcastContents = contents;

    if (definition->hazard != LegendsContainerDefinition::Hazard::None) {
        const bool poison = definition->hazard == LegendsContainerDefinition::Hazard::Poison;
        // Weapon type 20/21 select these 40-byte records at 8c125f00.
        const std::string effect = poison ? "POISONEXP" : "EXPLOSION";
        ItemInstance blast;
        blast.position = definition->position + definition->blastOffset;
        const usize index = m_fixtures.size();
        place(device, effect, blast);
        if (index == m_fixtures.size()) {
            definition->interactive = false;
            warn("Dreamcast barrel effect is unavailable: " + effect);
        } else {
            binding.blastFigure = index;
            m_hiddenFixtures[index] = true;
            const s32 active = sequence(index, "ACTIVE");
            if (active != 0 || m_fixtures[index].ticksOf(active) <= 0) {
                definition->interactive = false;
                warn("Dreamcast barrel effect has no native ACTIVE duration: " + effect);
            } else {
                binding.blastSeconds = static_cast<f32>(m_fixtures[index].ticksOf(active)) / 60;
                m_fixtures[index].play(active, ItemFigure::Playback::Once);
            }
        }
    }

    for (const auto key : m_dreamcastLoot.outcomes(contents)) {
        const auto& row = m_dreamcastLoot.definition(key);
        ContainerPresentation::Outcome outcome;
        outcome.key = static_cast<u16>(key);
        outcome.openingFigure = outcome.brokenFigure = idleFigure;
        outcome.openingSequence = opening;
        outcome.brokenSequence = broken;
        // Death and scripted contents need their native actor adapter. Keep
        // the container solid/unspent until every possible outcome is safe,
        // rather than replacing an unavailable outcome with empty loot.
        formats::ItemInstanceRecord placement;
        placement.info = key;
        placement.position = definition->position;
        auto pickup = legendsDreamcastPickup(row, placement, nativeIndex, realmType, &diagnostic);
        if (!pickup) {
            definition->interactive = false;
            warn("Dreamcast container contents deferred: " + row.name +
                 (diagnostic.empty() ? " (native actor or scripted item)"
                                     : " (" + diagnostic + ")"));
            continue;
        }
        const bool builtInGold =
            row.type == 1 && row.subtype == 1 && (info.subtype == 47 || info.subtype == 48);
        if (builtInGold && info.subtype == 48) {
            // 8c0ae55a replaces CHESTS with CHESTSG for gold. Its gold is
            // part of the tree; spawning TREAS_GOLD too doubles the artwork.
            ItemInstance goldPlacement;
            goldPlacement.position = instance.position;
            goldPlacement.rotation = instance.rotation;
            const usize gold = m_fixtures.size();
            place(device, definition->goldOpening, goldPlacement);
            if (m_fixtures.size() == gold || sequence(gold, "ACTIVE") < 0 ||
                sequence(gold, "OPEN") < 0) {
                if (m_fixtures.size() != gold)
                    m_hiddenFixtures[gold] = true;
                definition->interactive = false;
                warn("Dreamcast gold chest variant is unavailable: " + definition->goldOpening);
                continue;
            }
            m_hiddenFixtures[gold] = true;
            outcome.openingFigure = outcome.brokenFigure = gold;
            outcome.openingSequence = sequence(gold, "ACTIVE");
            outcome.brokenSequence = sequence(gold, "OPEN");
        }
        usize pickupFigure = outcome.openingFigure;
        if (!builtInGold) {
            ItemInstance lootPlacement;
            lootPlacement.position = definition->position;
            const usize figure = m_fixtures.size();
            place(device, row.name, lootPlacement);
            if (m_fixtures.size() == figure) {
                definition->interactive = false;
                warn("Dreamcast contained item artwork is unavailable: " + row.name);
                continue;
            }
            pickupFigure = figure;
            m_hiddenFixtures[figure] = true;
        }
        pickup->active = false;
        pickup->position = definition->position;
        const auto& box = definition->closedObstacle;
        pickup->height = box.height;
        pickup->radius =
            box.cylinderRadius > 0 ? box.cylinderRadius : std::hypot(box.halfAcross, box.halfAlong);
        if (box.cylinderRadius == 0) {
            pickup->collision = LegendsPickupCollision::Box;
            pickup->xSize = box.halfAcross;
            pickup->zSize = box.halfAlong;
            pickup->yaw = box.yaw;
        }
        outcome.pickup = m_pickups.size();
        m_pickups.push_back(std::move(*pickup));
        m_pickupFigures.push_back(pickupFigure);
        binding.outcomes.push_back(outcome);
    }
    binding.state = LegendsContainer{*definition};
    m_containers.push_back(std::move(binding));
}

void LegendsSceneAdapter::selectDreamcastContainer(usize index) {
    auto& binding = m_containers[index];
    if (!binding.dreamcastContents)
        return;
    const auto selected =
        m_dreamcastLoot.select(*binding.dreamcastContents, binding.state.definition().nativeIndex);
    if (!selected)
        return;
    const auto outcome = std::ranges::find(binding.outcomes, static_cast<u16>(*selected),
                                           &ContainerPresentation::Outcome::key);
    if (outcome == binding.outcomes.end())
        throw FormatError("Dreamcast container selected unbound contents");
    binding.openingFigure = outcome->openingFigure;
    binding.brokenFigure = outcome->brokenFigure;
    binding.openingSequence = outcome->openingSequence;
    binding.brokenSequence = outcome->brokenSequence;
    binding.pickup = outcome->pickup;
}

} // namespace gdl::game
