#include "game/world/DestructibleWalls.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/combat/Damage.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {
void DestructibleWalls::bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
                             TextureSet& textures, WorldCollision& collision) {
    clear();
    m_white = &device.whiteTexture();
    constexpr s32 kObstacle = 10;
    constexpr s32 kWall = 42;
    const auto& infos = layout.itemInfos();
    const auto& instances = layout.itemInstances();
    std::vector<CollisionTriangle> surfaces;
    for (usize index = 0; index < instances.size(); ++index) {
        const auto& instance = instances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const auto& info = infos[static_cast<usize>(instance.info)];
        const auto subtype =
            static_cast<s32>(instance.params[0]) | (static_cast<s32>(instance.params[1]) << 8);
        if (info.type != kObstacle || (subtype > 0 ? subtype : info.subtype) != kWall) {
            continue;
        }
        Wall wall;
        wall.health = info.hitPoints;
        wall.armor = info.armor;
        wall.minPlayers = instance.minPlayers;
        wall.object = static_cast<s32>(layout.objects().size() + index);
        wall.transform = itemPlacement(instance.position, instance.rotation);
        TreeInfo tree;
        TreeNodeInfo node;
        node.name = instance.name.empty() ? info.name : instance.name;
        node.object = node.name;
        node.objectFlags = info.objectFlags;
        tree.nodes.push_back(node);
        if (!wall.model.bind(tree, models, textures, device)) {
            log::warn("Destructible wall mesh missing: {}", node.name);
        }
        Vec3 low{0};
        Vec3 high{0};
        bool first = true;
        for (const auto& local : instance.collision) {
            CollisionTriangle triangle;
            triangle.object = wall.object;
            triangle.normal = glm::normalize(Mat3{wall.transform} * local.normal);
            for (usize i = 0; i < triangle.vertices.size(); ++i) {
                const Vec3 point{wall.transform * Vec4{local.vertices[i], 1}};
                triangle.vertices[i] = point;
                low = first ? point : glm::min(low, point);
                high = first ? point : glm::max(high, point);
                first = false;
            }
            wall.surface.push_back(triangle);
            surfaces.push_back(triangle);
        }
        if (wall.surface.empty()) {
            log::warn("Destructible wall {} has no exported collision triangles", node.name);
        }
        wall.bounds.base = Vec3{(low.x + high.x) * 0.5f, low.y, (low.z + high.z) * 0.5f};
        wall.bounds.radius = glm::length(Vec2{high.x - low.x, high.z - low.z}) * 0.5f;
        wall.bounds.height = high.y - low.y;
        m_walls.push_back(std::move(wall));
    }
    collision.append(surfaces);
    setPlayerCount(1, collision);
}

bool DestructibleWalls::standing(usize index) const {
    return index < m_walls.size() && m_walls[index].shown && m_walls[index].health > 0;
}

void DestructibleWalls::setPlayerCount(s32 players, WorldCollision& collision) {
    for (usize i = 0; i < m_walls.size(); ++i) {
        m_walls[i].shown = shownToParty(m_walls[i].minPlayers, players);
        collision.setSolid(m_walls[i].object, standing(i));
    }
}

MissileTarget DestructibleWalls::target(usize index, s32 id) const {
    const auto& wall = m_walls.at(index);
    auto result = wall.bounds;
    result.id = id;
    result.surface = wall.surface;
    return result;
}

std::optional<s32> DestructibleWalls::strike(usize index, f32 power, WorldCollision& collision,
                                             u32 flags) {
    if (!standing(index) || !std::isfinite(power) || power <= 0 || m_walls[index].armor < 0 ||
        (flags & Damage::kGas) != 0) {
        return std::nullopt;
    }
    auto& wall = m_walls[index];
    const f32 damage = std::min(static_cast<f32>(wall.health),
                                std::max(1.0f, power - static_cast<f32>(wall.armor)));
    wall.health -= static_cast<s32>(std::lround(damage));
    if (wall.health == 0) {
        collision.setSolid(wall.object, false);
    } else {
        wall.flash = 1.0f / 30.0f;
        wall.model.setMaskedTexture(m_white);
        wall.model.setAppearance(true);
    }
    return wall.health;
}

void DestructibleWalls::update(f32 seconds) {
    for (auto& wall : m_walls) {
        wall.flash = std::max(0.0f, wall.flash - seconds);
        if (wall.flash == 0) {
            wall.model.setMaskedTexture(nullptr);
            wall.model.setAppearance(false);
        }
    }
}

void DestructibleWalls::draw(RenderDevice& device, const Mat4& clip,
                             const WorldLighting& lighting) const {
    for (usize i = 0; i < m_walls.size(); ++i) {
        if (standing(i)) {
            m_walls[i].model.draw(device, clip, m_walls[i].transform, lighting);
        }
    }
}
} // namespace gdl::game
