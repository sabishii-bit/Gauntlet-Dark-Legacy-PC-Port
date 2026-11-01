#include <algorithm>
#include <stdexcept>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindN64Rider(
    const formats::LegendsN64Fixture& fixture,
    std::span<const formats::LegendsMapCollisionTriangle> collision, usize firstFigure,
    usize firstPickup, usize firstContainer, usize firstGate) {
    if (firstFigure > m_fixtures.size() || firstPickup > m_pickups.size() ||
        firstContainer > m_containers.size() || firstGate > m_gates.size()) {
        throw std::invalid_argument("Invalid native rider binding range");
    }
    // Switches already retain their supporting owner in LegendsWorldMotion.
    // Do not infer parenthood from proximity or attach unrelated trap effects.
    if (!fixture.groundProjected ||
        (firstPickup == m_pickups.size() && firstContainer == m_containers.size() &&
         firstGate == m_gates.size())) {
        return;
    }
    const auto placement = formats::legendsN64FixturePlacement(fixture, collision);
    if (placement.floorObject < 0) {
        return;
    }
    NativeRider rider;
    rider.owner = placement.floorObject;
    for (usize i = firstFigure; i < m_fixtures.size(); ++i) {
        rider.figures.push_back({i, m_fixtures[i].transform()});
    }
    for (usize i = firstPickup; i < m_pickups.size(); ++i) {
        rider.pickups.push_back({i, m_pickups[i].position});
    }
    for (usize i = firstContainer; i < m_containers.size(); ++i) {
        rider.containers.push_back({i, m_containers[i].state.definition().position});
    }
    for (usize i = firstGate; i < m_gates.size(); ++i) {
        rider.gates.push_back({i, m_gates[i].state.definition().position});
    }
    m_nativeRiders.push_back(std::move(rider));
}

void LegendsSceneAdapter::updateRiders() {
    for (auto& rider : m_nativeRiders) {
        // Native pickup/trap floor-owner references select one motion record.
        // They do not inherit all rendering parents, nor accumulate our last
        // applied offset each tick. Every outcome retains its original pose.
        const Vec3 offset = m_worldMovement.supportingOffset(rider.owner);
        const Vec3 delta = offset - rider.appliedOffset;
        if (delta != Vec3{0}) {
            for (const auto& figure : rider.figures) {
                auto& instance = m_fixtures[figure.index];
                // Chest contents can acquire a local rise/scale during opening.
                // Preserve that pose instead of restoring the closed baseline.
                instance.placeAt(glm::translate(Mat4{1}, delta) * instance.transform());
            }
        }
        rider.appliedOffset = offset;
        for (const auto& pickup : rider.pickups) {
            const Vec3 position = pickup.position + offset;
            m_pickups[pickup.index].position = position;
            for (auto& selector : m_floorSelectors) {
                if (selector.pickup == pickup.index) {
                    for (auto& outcome : selector.outcomes) {
                        if (outcome.pickup) {
                            outcome.pickup->position = position;
                        }
                    }
                }
            }
        }
        for (const auto& container : rider.containers) {
            m_containers[container.index].state.placeAt(container.position + offset);
        }
        for (const auto& gate : rider.gates) {
            m_gates[gate.index].state.placeAt(gate.position + offset);
        }
    }
}

} // namespace gdl::game
