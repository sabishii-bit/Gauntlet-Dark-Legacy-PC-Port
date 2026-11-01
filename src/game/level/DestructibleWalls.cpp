#include "game/level/DestructibleWalls.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/level/ItemFigure.h"
#include "game/party/TargetAssist.h"

namespace gdl::game {
bool DestructibleWalls::appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    digest.string("DestructibleWalls/1");
    digest.resource(m_layout);
    digest.resource(m_textures);
    digest.resource(m_collision);
    digest.resource(m_white);
    digest.wide(m_walls.size());
    for (usize i = 0; i < m_walls.size(); ++i) {
        const auto& wall = m_walls[i];
        digest.value(saved ? saved->m_walls[i] : static_cast<const WallState&>(wall));
        digest.values(wall.transform, wall.collisionCentre, wall.armor, wall.minPlayers,
                      wall.object);
        digest.wide(wall.surface.size());
        for (const auto& triangle : wall.surface) {
            digest.fields<4>(triangle);
        }
        const auto& [id, base, radius, height, surface, node, score, reach, potion, acquisition] =
            wall.bounds;
        digest.values(id, base, radius, height, node, score, reach, potion);
        digest.wide(surface.size());
        for (const auto& triangle : surface) {
            digest.fields<4>(triangle);
        }
        digest.flag(acquisition.has_value());
        if (acquisition) {
            digest.fields<5>(*acquisition);
        }
        // The model's white override is derived from WallState::white on restore.
    }
    return digest.good();
}

bool DestructibleWalls::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, nullptr);
}

bool DestructibleWalls::appendLocalDigest(NativeStateDigest& digest,
                                          const LocalSnapshot& saved) const {
    // Guard the native snapshot roster too; owner/epoch are admission, not peer state.
    [[maybe_unused]] const auto& [walls, binding, owner] = saved;
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, &saved);
}

void DestructibleWalls::saveLocal(LocalSnapshot& saved) const {
    saved.m_walls.resize(m_walls.size());
    for (usize index = 0; index < m_walls.size(); ++index) {
        saved.m_walls[index] = static_cast<const WallState&>(m_walls[index]);
    }
    saved.m_binding = m_checkpointBinding;
    saved.m_owner = this;
}

bool DestructibleWalls::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_checkpointBinding &&
           saved.m_binding.lock() == m_checkpointBinding && saved.m_walls.size() == m_walls.size();
}

bool DestructibleWalls::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_walls == right.m_walls;
}

bool DestructibleWalls::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    for (usize index = 0; index < m_walls.size(); ++index) {
        auto& wall = m_walls[index];
        static_cast<WallState&>(wall) = saved.m_walls[index];
        showWhite(wall, wall.white);
    }
    ++m_checkpointGeneration;
    return true;
}

void DestructibleWalls::clear() {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_layout = nullptr;
    m_models = nullptr;
    m_textures = nullptr;
    m_collision = nullptr;
    m_walls.clear();
}

void DestructibleWalls::showWhite(Wall& wall, bool white) {
    wall.white = white;
    wall.model.setMaskedTexture(white ? m_white : nullptr);
    wall.model.setAppearance(white);
}

void DestructibleWalls::bind(RenderDevice& device, const WorldLayout& layout, ModelSet& models,
                             TextureSet& textures, WorldCollision& collision) {
    clear();
    m_layout = &layout;
    m_models = &models;
    m_textures = &textures;
    m_collision = &collision;
    m_white = &device.whiteTexture();
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
        if (info.type != ItemInfo::kObstacle || (subtype > 0 ? subtype : info.subtype) != kWall) {
            continue;
        }
        Wall wall;
        wall.health = info.hitPoints;
        wall.armor = info.armor;
        wall.minPlayers = instance.minPlayers;
        wall.object = static_cast<s32>(layout.objects().size() + index);
        wall.transform = itemPlacement(instance.position, instance.rotation);
        wall.bounds.acquisition =
            TargetAssist::itemAcquisition(wall.transform, info.collisionOffset, info.radius,
                                          info.height, TargetAssist::kItemDistanceScale);
        wall.bounds.acquisition->enabled = wall.armor != -1;
        wall.collisionCentre = wall.bounds.acquisition->point;
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
        (flags & damage::kGas) != 0) {
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
        showWhite(wall, true);
    }
    return wall.health;
}

void DestructibleWalls::reveal(usize index) {
    if (standing(index)) {
        m_walls[index].blinks = kRevealBlinks;
    }
}

void DestructibleWalls::update(f32 seconds) {
    for (auto& wall : m_walls) {
        wall.flash = std::max(0.0f, wall.flash - seconds);
        bool white = wall.flash > 0;
        if (wall.blinks > 0) {
            white = white || wall.blinkTicks < kBlinkWhite;
            wall.blinkTicks += seconds * kTicksPerSecond;
            if (wall.blinkTicks > kBlinkCycle) {
                wall.blinkTicks = 0;
                --wall.blinks;
            }
        }
        showWhite(wall, white);
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
