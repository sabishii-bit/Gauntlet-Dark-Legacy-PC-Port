#include "game/level/LockedGates.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/level/NativeStateResources.h"

namespace gdl::game {

bool LockedGates::appendSimulationDigest(NativeStateDigest& digest, const Simulation& state) const {
    digest.string("LockedGates.native.1");
    // These immutable binding inputs remain fixed until clear/bind invalidates
    // history. Mutable collision values are visited by the enclosing world owner.
    digest.resource(m_layout);
    digest.resource(m_collision);
    digest.fields<1>(state);
    return digest.good();
}

bool LockedGates::appendNativeDigest(NativeStateDigest& digest) const {
    return appendSimulationDigest(digest, m_sim);
}

bool LockedGates::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendSimulationDigest(digest, saved.m_state);
}

bool LockedGates::registerNativeResources(NativeStateResources& resources) const {
    for (const auto& gate : m_sim.gates) {
        if (!gate.figure.registerNativeResources(resources)) {
            return false;
        }
    }
    return resources.good();
}

void LockedGates::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_binding = m_checkpointBinding;
    saved.m_owner = this;
}

bool LockedGates::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_checkpointBinding &&
           saved.m_binding.lock() == m_checkpointBinding;
}

bool LockedGates::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_state == right.m_state;
}

bool LockedGates::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
}

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

bool LockedGates::bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope,
                       const WorldCollision* collision) {
    clear();
    m_collision = collision;
    m_layout = &layout;
    const auto authored = itemSupportWorld(layout, collision);
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            infos[static_cast<usize>(instance.info)].type != ItemInfo::kGate) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        Gate gate;
        gate.instance = static_cast<s32>(index);
        gate.subtype = info.subtype;
        gate.minPlayers = instance.minPlayers;
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        if (!gate.figure.place(device, scope, name, instance,
                               (info.collisionFlags & ItemInfo::kKeepsHeight) != 0 ? nullptr
                                                                                   : collision)) {
            log::warn("Gates: no figure {} in the item archive", name);
        }
        gate.box = gate.figure.obstacle(info);
        gate.support.bind(instance, info, authored ? &*authored : nullptr, gate.figure, gate.box,
                          collision);
        gate.support.sync(collision, gate.figure, gate.box);
        if (info.collisionType == 4) {
            // Type 7 also includes floor decorations (E1DOORCARPET23). Retail
            // fn_8005FDA8 tests the authored triangle list, not the broad-phase
            // radius as a solid box. A floor-only list cannot block a passage
            // and must never reach the key-spending contact handler.
            const Mat3 rotation{itemPlacement(Vec3{0}, instance.rotation)};
            gate.blocksPassage = std::ranges::any_of(instance.collision, [&](const auto& triangle) {
                return std::abs((rotation * triangle.normal).y) < WorldCollision::kFloorNormalY;
            });
            gate.box.solid = gate.blocksPassage;
            if (instance.collision.empty()) {
                log::warn("Gates: {} has no exported triangle collision; re-export the level",
                          name);
            }
        }
        m_sim.gates.push_back(std::move(gate));
    }
    return !m_sim.gates.empty();
}

void LockedGates::clear() {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_collision = nullptr;
    m_layout = nullptr;
    m_sim.gates.clear();
}

void LockedGates::syncFloors() {
    for (auto& gate : m_sim.gates) {
        gate.support.sync(m_collision, gate.figure, gate.box);
    }
}

void LockedGates::setPlayerCount(s32 players) {
    for (auto& gate : m_sim.gates) {
        gate.shown = shownToParty(gate.minPlayers, players);
    }
}

std::vector<GateEvent> LockedGates::update(s32 ticks, f32 seconds,
                                           std::span<const ChestVisitor> party) {
    std::vector<GateEvent> events;
    syncFloors();
    for (usize index = 0; index < m_sim.gates.size(); ++index) {
        Gate& gate = m_sim.gates[index];
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
                // The key spent, the door blocks nobody from this moment: PlayerCollideItem's
                // door case bars only an unspent one.
                gate.box.solid = false;
                gate.figure.play(kOpening, ItemFigure::Playback::Once);
                events.push_back(event);
                break;
            }
        } else if (gate.state == kOpening) {
            gate.openingTicks += ticks;
            if (gate.figure.finished()) {
                gate.state = kOpen;
                gate.figure.play(kOpen, ItemFigure::Playback::Loop);
            }
        }
    }
    return events;
}

std::vector<Obstacle> LockedGates::obstacles() const {
    std::vector<Obstacle> boxes;
    for (const auto& gate : m_sim.gates) {
        if (gate.shown && gate.box.solid) {
            boxes.push_back(gate.box);
        }
    }
    return boxes;
}

std::vector<Obstacle> LockedGates::shotStops() const {
    // fn_8005F0F4 keeps a door in a shot's way until it has swung open.
    std::vector<Obstacle> boxes;
    for (const auto& gate : m_sim.gates) {
        if (gate.shown && gate.blocksPassage && gate.state != kOpen) {
            Obstacle box = gate.box;
            box.solid = true;
            boxes.push_back(box);
        }
    }
    return boxes;
}

void LockedGates::capturePresentation() {
    for (auto& gate : m_sim.gates) {
        gate.figure.capturePresentation();
    }
}

void LockedGates::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       f32 presentationAlpha) const {
    for (const auto& gate : m_sim.gates) {
        if (gate.shown) {
            gate.figure.draw(device, clip, lighting, 1, 1, nullptr, TreeModel::Pass::All,
                             presentationAlpha);
        }
    }
}

bool LockedGates::unlock(usize index) {
    if (index >= m_sim.gates.size()) {
        return false;
    }
    Gate& gate = m_sim.gates[index];
    if (!gate.shown || gate.state != kShut) {
        return false;
    }
    gate.state = kOpening;
    gate.openingTicks = 0;
    gate.box.solid = false;
    gate.figure.play(kOpening, ItemFigure::Playback::Once);
    return true;
}

bool LockedGates::lock(usize index) {
    if (index >= m_sim.gates.size()) {
        return false;
    }
    Gate& gate = m_sim.gates[index];
    if (!gate.shown || gate.state == kShut) {
        return false;
    }
    gate.state = kShut;
    gate.openingTicks = 0;
    gate.refusalLeft = 0.0f;
    gate.box.solid = gate.blocksPassage;
    gate.figure.play(kShut, ItemFigure::Playback::Loop);
    return true;
}

} // namespace gdl::game
