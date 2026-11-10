#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::bindDreamcastRider(const formats::ItemInfoRecord& info,
                                             const formats::ItemInstanceRecord& instance,
                                             usize firstFigure, usize firstPickup,
                                             usize firstContainer, usize firstGate) {
    if (!m_dreamcastMovement || (info.collisionFlags & 1) != 0 ||
        (firstPickup == m_pickups.size() && firstContainer == m_containers.size() &&
         firstGate == m_gates.size())) {
        return;
    }
    const auto floor = m_dreamcastMovement->supportAt(instance.position);
    if (!floor || !m_dreamcastMovement->carriesItems(floor->object)) {
        return;
    }
    DreamcastRider rider;
    auto& contents = rider.contents;
    contents.owner = floor->object;
    // The shared loader has already run native FloorPos and added its .1 lift.
    // AddItemSub then reparents to this owner while preserving that world
    // placement. In B1, RUNEB1 rides elevator floor231, not the independently
    // animated decorative pedestal tracks243..245.
    for (usize i = firstFigure; i < m_fixtures.size(); ++i) {
        contents.figures.push_back({i, m_fixtures[i].transform()});
    }
    for (usize i = firstPickup; i < m_pickups.size(); ++i) {
        contents.pickups.push_back({i, m_pickups[i].position});
    }
    for (usize i = firstContainer; i < m_containers.size(); ++i) {
        contents.containers.push_back({i, m_containers[i].state.definition().position});
    }
    for (usize i = firstGate; i < m_gates.size(); ++i) {
        contents.gates.push_back({i, m_gates[i].state.definition().position});
    }
    m_dreamcastRiders.push_back(std::move(rider));
}

void LegendsSceneAdapter::updateDreamcastRiders() {
    if (!m_dreamcastMovement) {
        return;
    }
    for (auto& rider : m_dreamcastRiders) {
        const Mat4 placement = m_dreamcastMovement->riderTransform(rider.contents.owner);
        if (placement == rider.applied) {
            continue;
        }
        for (const auto& binding : rider.contents.figures) {
            auto& figure = m_fixtures[binding.index];
            // AddItemSub8c0a99e0 reparents the root, and UpdateObjWorldMat
            // supplies its local matrix. Compose from that fixed binding,
            // never invert last tick's possibly collapsed platform scale.
            // Chest contents apply their opening-local pose afterward.
            figure.placeAt(placement * binding.placement);
        }
        for (const auto& binding : rider.contents.pickups) {
            m_pickups[binding.index].position = Vec3{placement * Vec4{binding.position, 1}};
        }
        for (const auto& binding : rider.contents.containers) {
            m_containers[binding.index].state.placeAt(Vec3{placement * Vec4{binding.position, 1}});
        }
        for (const auto& binding : rider.contents.gates) {
            m_gates[binding.index].state.placeAt(Vec3{placement * Vec4{binding.position, 1}});
        }
        rider.applied = placement;
    }
}

} // namespace gdl::game
