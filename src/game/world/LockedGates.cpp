#include "game/world/LockedGates.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

std::string_view LockedGates::openingSound(s32 realm, s32 subtype) {
    static constexpr std::array<std::array<std::string_view, 4>, 14> kSounds{{
        {},
        {"S_GATEA4", "S_GATEA2", "S_GATEA3", "S_GATEA1"},
        {"S_GATEB1", "S_GATEB1", "S_GATEB1", "S_GATEB1"},
        {"S_GATEC1", "S_GATEC2", "S_GATEC3", "S_GATEC1"},
        {"S_GATED1", "S_GATED1", "S_GATED1", "S_GATED1"},
        {},
        {},
        {"S_GATEWOODG", "S_GATEWOODG", "S_GATEWOODG", "S_GATEMETG"},
        {"S_GATEWOODH", "S_GATEWOODH", "S_GATEWOODH", "S_GATEWOODH"},
        {"S_GATEWOODI", "S_GATEWOODI", "S_GATEWOODI", "S_GATEWOODI"},
        {"S_GATEWOODJ", "S_GATEWOODJ", "S_GATEWOODJ", "S_GATEMETJ"},
        {"S_GATEWOODK", "S_GATEWOODK", "S_GATEWOODK", "S_GATEWOODK"},
        {},
        {},
    }};
    if (realm < 0 || static_cast<usize>(realm) >= kSounds.size() || subtype < 0 || subtype >= 4) {
        return {};
    }
    return kSounds[static_cast<usize>(realm)][static_cast<usize>(subtype)];
}

bool LockedGates::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                       const WorldCollision* collision, ItemArchive* realmItems) {
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
        gate->subtype = info.subtype;
        gate->minPlayers = instance.minPlayers;
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        ItemArchive& art = itemArchiveForTree(items, name, realmItems);
        if (!gate->figure.place(device, art, name, instance, collision)) {
            log::warn("Gates: no figure {} in the item archive", name);
        }
        gate->box = gate->figure.obstacle(info);
        if (info.collisionType == 4) {
            // Type 7 also includes floor decorations (E1DOORCARPET23). Retail
            // fn_8005FDA8 tests the authored triangle list, not the broad-phase
            // radius as a solid box. A floor-only list cannot block a passage
            // and must never reach the key-spending contact handler.
            const Mat3 rotation{itemPlacement(Vec3{0}, instance.rotation)};
            gate->blocksPassage =
                std::ranges::any_of(instance.collision, [&](const auto& triangle) {
                    return std::abs((rotation * triangle.normal).y) < WorldCollision::kFloorNormalY;
                });
            gate->box.solid = gate->blocksPassage;
            if (instance.collision.empty()) {
                log::warn("Gates: {} has no exported triangle collision; re-export the level",
                          name);
            }
        }
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
        if (!gate.blocksPassage) {
            continue;
        }
        gate.refusalLeft = std::max(gate.refusalLeft - seconds, 0.0f);
        if (gate.state == kShut) {
            for (usize v = 0; v < party.size(); ++v) {
                if (!gate.box.touchedBy(party[v].position, party[v].radius)) {
                    continue;
                }
                // Backing away from it neither spends a key nor is refused (ItemTouch).
                const Vec3 toward = gate.figure.position() - party[v].position;
                if (party[v].step.x * toward.x + party[v].step.z * toward.z < 0.0f) {
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
