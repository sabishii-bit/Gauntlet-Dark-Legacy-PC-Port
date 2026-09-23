#include "game/world/SafeRocks.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {

std::int32_t parameter(const ItemInstance& instance, std::size_t offset) {
    // The unpacked manifest keeps the original little-endian parameter bytes.
    const std::uint32_t value = static_cast<std::uint32_t>(instance.params[offset]) |
                                (static_cast<std::uint32_t>(instance.params[offset + 1]) << 8);
    return value < 0x8000U ? static_cast<std::int32_t>(value)
                           : static_cast<std::int32_t>(value) - 0x10000;
}

} // namespace

bool SafeRocks::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items) {
    clear();
    const auto& infos = layout.itemInfos();
    const auto& instances = layout.itemInstances();
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<std::size_t>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<std::size_t>(instance.info)];
        const std::int32_t override = parameter(instance, 0);
        if (info.type != kItemType || (override > 0 ? override : info.subtype) != kSubtype) {
            continue;
        }
        auto rock = std::make_unique<Rock>();
        rock->instance = static_cast<std::int32_t>(i);
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
        for (std::int32_t tier = 0; tier <= kWhole; ++tier) {
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
                found = rock->models[static_cast<std::size_t>(tier)].bind(tree, items.models,
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

void SafeRocks::setPlayerCount(std::int32_t players) {
    for (const auto& rock : m_rocks) {
        rock->shown = shownToParty(rock->minPlayers, players);
    }
}

bool SafeRocks::standing(std::size_t index) const {
    return index < m_rocks.size() && m_rocks[index]->shown && m_rocks[index]->health > 0 &&
           m_rocks[index]->tier > 0;
}

bool SafeRocks::strike(std::size_t index, float power) {
    if (!standing(index) || m_rocks[index]->armor < 0 || !std::isfinite(power) || power <= 0.0f) {
        return false;
    }
    Rock& rock = *m_rocks[index];
    const float damage =
        std::clamp(power - static_cast<float>(rock.armor), 1.0f, static_cast<float>(rock.health));
    rock.health -= static_cast<std::int32_t>(std::lround(damage));
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

void SafeRocks::activate(std::size_t index) {
    if (index < m_rocks.size()) {
        Rock& rock = *m_rocks[index];
        rock.health = kWhole * rock.baseHealth;
        rock.tier = kWhole;
    }
}

std::vector<Obstacle> SafeRocks::obstacles() const {
    std::vector<Obstacle> out;
    for (std::size_t i = 0; i < size(); ++i) {
        if (standing(i)) {
            out.push_back(m_rocks[i]->obstacle);
        }
    }
    return out;
}

void SafeRocks::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    for (const auto& rock : m_rocks) {
        if (rock->shown) {
            rock->models[static_cast<std::size_t>(rock->tier)].draw(device, clip, rock->placement,
                                                                    lighting);
        }
    }
}

} // namespace gdl::game
