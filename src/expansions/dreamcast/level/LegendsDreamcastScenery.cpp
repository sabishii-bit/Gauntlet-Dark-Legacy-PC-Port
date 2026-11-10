#include "expansions/dreamcast/level/LegendsDreamcastScenery.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/core/Error.h"

#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "game/combat/DamageTypes.h"

namespace gdl::game {
namespace {
bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

void LegendsDreamcastScenery::add(const formats::ItemInfoRecord& info,
                                  const formats::ItemInstanceRecord& item, usize figure, s32 object,
                                  std::span<const formats::WorldCollisionTriangle> faces) {
    if (info.type != 10 || info.subtype != 42 || info.collisionType != 4 || info.hitPoints <= 0 ||
        info.armor < 0 || object < 0 || !finite(item.position) || !finite(item.rotation) ||
        item.triangleIndex < 0 || item.triangleCount <= 0 ||
        static_cast<usize>(item.triangleIndex) > faces.size() ||
        static_cast<usize>(item.triangleCount) > faces.size() - item.triangleIndex ||
        m_walls.size() >= 1024 || targetForObject(object)) {
        throw FormatError("Invalid Dreamcast destructible item");
    }
    Wall wall;
    wall.figure = figure;
    wall.object = object;
    wall.health = info.hitPoints;
    wall.armor = info.armor;
    wall.minimum = Vec3{std::numeric_limits<f32>::max()};
    wall.maximum = -wall.minimum;
    const Mat4 transform = legendsDreamcastItemPlacement(item.position, item.rotation);
    for (const auto& face : faces.subspan(item.triangleIndex, item.triangleCount)) {
        CollisionTriangle placed;
        placed.object = object;
        placed.normal = Vec3{transform * Vec4{face.normal, 0}};
        if (!finite(placed.normal) || glm::length(placed.normal) < .0001f) {
            throw FormatError("Invalid Dreamcast destructible normal");
        }
        placed.normal = glm::normalize(placed.normal);
        for (usize i = 0; i < 3; ++i) {
            placed.vertices[i] = Vec3{transform * Vec4{face.vertices[i], 1}};
            if (!finite(placed.vertices[i]) ||
                glm::any(glm::greaterThan(glm::abs(placed.vertices[i]), Vec3{100000}))) {
                throw FormatError("Invalid Dreamcast destructible vertex");
            }
            wall.minimum = glm::min(wall.minimum, placed.vertices[i]);
            wall.maximum = glm::max(wall.maximum, placed.vertices[i]);
        }
        wall.surface.push_back(placed);
    }
    m_walls.push_back(std::move(wall));
}

std::vector<ImportedTarget> LegendsDreamcastScenery::targets() const {
    std::vector<ImportedTarget> result;
    for (usize i = 0; i < m_walls.size(); ++i) {
        const auto& wall = m_walls[i];
        if (wall.health <= 0) {
            continue;
        }
        const Vec3 middle = (wall.minimum + wall.maximum) * .5f;
        result.push_back({kImportedSceneryBase + static_cast<s32>(i),
                          {middle.x, wall.minimum.y, middle.z},
                          std::max(.001f, glm::length(Vec2{wall.maximum.x - middle.x,
                                                           wall.maximum.z - middle.z})),
                          std::max(.001f, wall.maximum.y - wall.minimum.y),
                          true,
                          true,
                          wall.surface});
    }
    return result;
}

bool LegendsDreamcastScenery::strike(s32 target, f32 damage, u32 flags) {
    if (target < kImportedSceneryBase ||
        static_cast<usize>(target - kImportedSceneryBase) >= m_walls.size() ||
        !std::isfinite(damage) || damage <= 0 || (flags & (damage::kMagic | damage::kGas)) != 0) {
        return false;
    }
    auto& wall = m_walls[static_cast<usize>(target - kImportedSceneryBase)];
    if (wall.health <= 0) {
        return false;
    }
    // HitItem 8c0afaa0 subtracts armor, clamps damage to at least 1, then
    // truncates to native integer HP. Case42 calls DeleteItem on the final hit.
    const f32 amount =
        std::clamp(damage - static_cast<f32>(wall.armor), 1.0f, static_cast<f32>(wall.health));
    wall.health -= static_cast<s32>(amount);
    // ProcessItem 8c0acf00 resets the override on the following native tick.
    wall.flash = wall.health > 0 ? 1.0f / 30 : 0;
    return true;
}

std::optional<s32> LegendsDreamcastScenery::targetForObject(s32 object) const {
    for (usize i = 0; i < m_walls.size(); ++i) {
        if (m_walls[i].object == object) {
            return kImportedSceneryBase + static_cast<s32>(i);
        }
    }
    return std::nullopt;
}

void LegendsDreamcastScenery::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    for (auto& wall : m_walls) {
        wall.flash = std::max(0.0f, wall.flash - seconds);
    }
}

bool LegendsDreamcastScenery::visible(usize figure) const {
    for (const auto& wall : m_walls) {
        if (wall.figure == figure) {
            return wall.health > 0;
        }
    }
    return true;
}

f32 LegendsDreamcastScenery::whiteBlend(usize figure) const {
    for (const auto& wall : m_walls) {
        if (wall.figure == figure) {
            return wall.flash > 0 ? 1.0f : 0.0f;
        }
    }
    return 0;
}

s32 LegendsDreamcastScenery::health(s32 target) const {
    if (target < kImportedSceneryBase ||
        static_cast<usize>(target - kImportedSceneryBase) >= m_walls.size()) {
        return 0;
    }
    return m_walls[static_cast<usize>(target - kImportedSceneryBase)].health;
}

Vec3 LegendsDreamcastScenery::position(s32 target) const {
    if (target < kImportedSceneryBase ||
        static_cast<usize>(target - kImportedSceneryBase) >= m_walls.size()) {
        return {};
    }
    const auto& wall = m_walls[static_cast<usize>(target - kImportedSceneryBase)];
    return (wall.minimum + wall.maximum) * .5f;
}

void LegendsDreamcastScenery::bindCollision(WorldCollision& collision) const {
    for (const auto& wall : m_walls) {
        collision.append(wall.surface);
    }
    syncCollision(collision);
}

void LegendsDreamcastScenery::syncCollision(WorldCollision& collision) const {
    for (const auto& wall : m_walls) {
        collision.setSolid(wall.object, wall.health > 0);
    }
}
} // namespace gdl::game
