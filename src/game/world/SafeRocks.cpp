#include "game/world/SafeRocks.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

namespace {

s32 parameter(const ItemInstance& instance, usize offset) {
    // The unpacked manifest keeps the original little-endian parameter bytes.
    const u32 value = static_cast<u32>(instance.params[offset]) |
                      (static_cast<u32>(instance.params[offset + 1]) << 8);
    return value < 0x8000U ? static_cast<s32>(value) : static_cast<s32>(value) - 0x10000;
}

} // namespace

bool SafeRocks::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items) {
    clear();
    const auto& infos = layout.itemInfos();
    const auto& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        const s32 override = parameter(instance, 0);
        if (info.type != kItemType || (override > 0 ? override : info.subtype) != kSubtype) {
            continue;
        }
        auto rock = std::make_unique<Rock>();
        rock->instance = static_cast<s32>(i);
        rock->baseHealth = std::max(info.hitPoints, 0);
        rock->tier = std::clamp(parameter(instance, 2), 0, kWhole);
        rock->health = rock->tier * rock->baseHealth;
        rock->armor = info.armor;
        rock->minPlayers = instance.minPlayers;
        rock->position = instance.position;
        rock->placement = itemPlacement(instance.position, instance.rotation);
        rock->obstacle.centre = Vec3{rock->placement * Vec4{info.collisionOffset, 1.0f}};
        rock->obstacle.height = info.height;
        rock->obstacle.yaw = instance.rotation.y;
        rock->obstacle.halfAcross = info.xSize > 0.0f ? info.xSize : info.radius;
        rock->obstacle.halfAlong = info.zSize > 0.0f ? info.zSize : info.radius;
        if (info.collisionType == 1) {
            rock->obstacle.cylinderRadius = info.radius;
        }
        for (s32 tier = 0; tier <= kWhole; ++tier) {
            const std::string base = std::format("{}{}", info.name, tier);
            bool found = false;
            for (const char* suffix : {"", "L1", "L1ROOT"}) {
                const auto index = items.models.find(base + suffix);
                if (!index.has_value()) {
                    continue;
                }
                TreeInfo tree;
                tree.name = base;
                TreeNodeInfo node;
                node.name = base;
                node.object = items.models.entry(*index).name;
                node.objectFlags = info.objectFlags;
                tree.nodes.push_back(node);
                found = rock->models[static_cast<usize>(tier)].bind(tree, items.models,
                                                                    items.textures, device);
                break;
            }
            if (!found) {
                log::warn("Safe rocks: no model for {}", base);
            }
        }
        m_rocks.push_back(std::move(rock));
    }
    return !m_rocks.empty();
}

void SafeRocks::clear() {
    m_rocks.clear();
}

void SafeRocks::setPlayerCount(s32 players) {
    for (const auto& rock : m_rocks) {
        rock->shown = shownToParty(rock->minPlayers, players);
    }
}

bool SafeRocks::standing(usize index) const {
    return index < m_rocks.size() && m_rocks[index]->shown && !m_rocks[index]->dormant &&
           m_rocks[index]->health > 0 && m_rocks[index]->tier > 0;
}

bool SafeRocks::strike(usize index, f32 power) {
    if (!standing(index) || m_rocks[index]->armor < 0 || !std::isfinite(power) || power <= 0.0f) {
        return false;
    }
    Rock& rock = *m_rocks[index];
    const f32 damage =
        std::clamp(power - static_cast<f32>(rock.armor), 1.0f, static_cast<f32>(rock.health));
    rock.health -= static_cast<s32>(std::lround(damage));
    if (rock.health == 0) {
        rock.tier = 0;
    } else if (rock.health <= rock.baseHealth) {
        rock.tier = 1;
    } else if (rock.health <= 2 * rock.baseHealth) {
        rock.tier = 2;
    } else {
        rock.tier = kWhole;
    }
    return rock.health == 0;
}

void SafeRocks::activate(usize index) {
    if (index < m_rocks.size()) {
        Rock& rock = *m_rocks[index];
        rock.health = kWhole * rock.baseHealth;
        rock.tier = kWhole;
        rock.dormant = false;
        rock.activationDelay = 0;
    }
}

void SafeRocks::hideForEruptions() {
    constexpr usize kMaxAnchors = 16;
    for (usize i = 0; i < std::min(size(), kMaxAnchors); ++i) {
        m_rocks[i]->dormant = true;
        m_rocks[i]->activationDelay = 0;
    }
}

void SafeRocks::scheduleActivation(usize index, f32 delay) {
    if (index < size() && std::isfinite(delay) && delay > 0) {
        m_rocks[index]->activationDelay = delay;
    }
}

void SafeRocks::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    for (usize i = 0; i < size(); ++i) {
        auto& rock = *m_rocks[i];
        if (rock.activationDelay > 0) {
            rock.activationDelay -= seconds;
            if (rock.activationDelay <= 0) {
                activate(i);
            }
        }
    }
}

std::vector<CombatArenaTarget> SafeRocks::eruptionTargets() const {
    constexpr usize kMaxAnchors = 16;
    std::vector<CombatArenaTarget> targets;
    for (usize i = 0; i < std::min(size(), kMaxAnchors); ++i) {
        if (m_rocks[i]->shown && !standing(i)) {
            targets.push_back({i, m_rocks[i]->placement});
        }
    }
    return targets;
}

std::vector<Obstacle> SafeRocks::obstacles() const {
    std::vector<Obstacle> out;
    for (usize i = 0; i < size(); ++i) {
        if (standing(i)) {
            out.push_back(m_rocks[i]->obstacle);
        }
    }
    return out;
}

std::vector<Mat4> SafeRocks::attackAnchors() const {
    constexpr usize kMaxAnchors = 16;
    std::vector<Mat4> anchors;
    for (const auto& rock : m_rocks) {
        if (rock->shown) {
            anchors.push_back(rock->placement);
            if (anchors.size() == kMaxAnchors) {
                break;
            }
        }
    }
    return anchors;
}

void SafeRocks::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const auto& rock : m_rocks) {
        if (rock->shown && !rock->dormant) {
            rock->models[static_cast<usize>(rock->tier)].draw(device, clip, rock->placement,
                                                              lighting);
        }
    }
}

bool SafeRocks::blocksBreath(const Vec3& from, const Vec3& to) const {
    constexpr f32 kBreathProbeRadius = 0.5f;
    return blocksSegment(from, to, kBreathProbeRadius);
}

bool SafeRocks::blocksSegment(const Vec3& from, const Vec3& to, f32 radius) const {
    for (usize i = 0; i < m_rocks.size(); ++i) {
        if (standing(i) && m_rocks[i]->obstacle.blocksSegment(from, to, radius)) {
            return true;
        }
    }
    return false;
}

} // namespace gdl::game
