#include "game/world/LockedGates.h"

#include <algorithm>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

bool LockedGates::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                       const WorldCollision* collision) {
    clear();
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            infos[static_cast<usize>(instance.info)].type != ItemInfo::kGate) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        auto gate = std::make_unique<Gate>();
        gate->instance = static_cast<s32>(index);
        gate->minPlayers = instance.minPlayers;
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        if (!gate->figure.place(device, items, name, instance, collision)) {
            log::warn("Gates: no figure {} in the item archive", name);
        }
        gate->box = gate->figure.obstacle(info);
        m_gates.push_back(std::move(gate));
    }
    return !m_gates.empty();
}

void LockedGates::clear() {
    m_gates.clear();
}

void LockedGates::setPlayerCount(s32 players) {
    for (const std::unique_ptr<Gate>& gate : m_gates) {
        gate->shown = shownToParty(gate->minPlayers, players);
    }
}

std::vector<GateEvent> LockedGates::update(s32 ticks, f32 seconds,
                                           std::span<const ChestVisitor> party) {
    std::vector<GateEvent> events;
    for (usize index = 0; index < m_gates.size(); ++index) {
        Gate& gate = *m_gates[index];
        if (!gate.shown) {
            continue;
        }
        gate.figure.update(seconds);
        gate.refusalLeft = std::max(gate.refusalLeft - seconds, 0.0f);
        if (gate.state == kShut) {
            for (usize v = 0; v < party.size(); ++v) {
                if (!gate.box.touchedBy(party[v].position, party[v].radius)) {
                    continue;
                }
                GateEvent event{GateEvent::Kind::Unlocked, index, v, gate.figure.position()};
                if (party[v].keys <= 0) {
                    if (gate.refusalLeft <= 0.0f) {
                        gate.refusalLeft = kRefusalSeconds;
                        event.kind = GateEvent::Kind::Refused;
                        events.push_back(event);
                    }
                    continue;
                }
                gate.state = kOpening;
                gate.openingTicks = 0;
                gate.figure.play(kOpening, false);
                events.push_back(event);
                break;
            }
        } else if (gate.state == kOpening) {
            gate.openingTicks += ticks;
            gate.box.solid = gate.openingTicks <= kPassableTicks;
            if (gate.figure.finished()) {
                gate.state = kOpen;
                gate.box.solid = false;
                gate.figure.play(kOpen, true);
            }
        }
    }
    return events;
}

std::vector<Obstacle> LockedGates::obstacles() const {
    std::vector<Obstacle> boxes;
    for (const std::unique_ptr<Gate>& gate : m_gates) {
        if (gate->shown && gate->box.solid) {
            boxes.push_back(gate->box);
        }
    }
    return boxes;
}

void LockedGates::draw(RenderDevice& device, const Mat4& clip,
                       const WorldLighting& lighting) const {
    for (const std::unique_ptr<Gate>& gate : m_gates) {
        if (gate->shown) {
            gate->figure.draw(device, clip, lighting);
        }
    }
}

} // namespace gdl::game
