#include "expansions/n64/level/LegendsScenery.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "game/combat/DamageTypes.h"

namespace gdl::game {
LegendsScenery::LegendsScenery(std::span<const formats::LegendsMapDestructible> definitions,
                               f32 stageScale,
                               std::span<const formats::LegendsMapCollisionTriangle> faces)
    : m_definitions(definitions.begin(), definitions.end()) {
    if (!std::isfinite(stageScale) || stageScale <= 0 || stageScale > 100 ||
        definitions.size() > 64 || faces.size() > 32768) {
        throw std::invalid_argument("Invalid native destructible roster");
    }
    // e003955c initializes every SWAL health slot to trunc(stageScale * 30).
    const s32 health = static_cast<s32>(stageScale * 30);
    for (const auto& definition : definitions) {
        if (glm::any(glm::isnan(definition.minimum)) || glm::any(glm::isinf(definition.minimum)) ||
            glm::any(glm::isnan(definition.maximum)) || glm::any(glm::isinf(definition.maximum)) ||
            !glm::all(glm::greaterThanEqual(definition.maximum, definition.minimum))) {
            throw std::invalid_argument("Invalid native destructible bounds");
        }
        m_health.push_back(health);
        m_flash.push_back(0);
        auto& surface = m_surfaces.emplace_back();
        for (const auto& face : faces) {
            if (face.owner != static_cast<s32>(definition.object) || (face.flags & 0x800U) == 0 ||
                (!face.wall && !face.floor)) {
                continue;
            }
            for (const auto& vertex : face.vertices) {
                if (glm::any(glm::isnan(vertex)) || glm::any(glm::isinf(vertex)) ||
                    glm::any(glm::greaterThan(glm::abs(vertex), Vec3{100000}))) {
                    throw std::invalid_argument("Invalid native destructible surface");
                }
            }
            if (glm::any(glm::isnan(face.normal)) || glm::any(glm::isinf(face.normal)) ||
                glm::length(face.normal) < .0001f) {
                throw std::invalid_argument("Invalid native destructible surface normal");
            }
            surface.push_back({face.normal, face.vertices, face.owner, 0});
        }
    }
}

std::vector<ImportedTarget> LegendsScenery::targets() const {
    std::vector<ImportedTarget> result;
    for (usize i = 0; i < m_health.size(); ++i) {
        if (m_health[i] <= 0) {
            continue;
        }
        const auto& definition = m_definitions[i];
        // e003c0e8 adds a two-unit attack cylinder at the authored bounds centre,
        // from midpointY-.5 through midpointY+.5. Physical world faces remain
        // the movement/projectile blocker; this body is only an attack target.
        const Vec3 middle = (definition.minimum + definition.maximum) * .5f;
        ImportedTarget target{
            kTargetBase + static_cast<s32>(i), middle - Vec3{0, .5f, 0}, 2, 1, true, true};
        if (!m_surfaces[i].empty()) {
            // Reuse GDL's physical surface targeting: the small native attack
            // cylinder can lie behind the wall's own visible/collision mesh.
            // Every current SWAL owner is static (none has motion flag8000),
            // and decoded CTRI vertices already occupy world space.
            target.base = {middle.x, definition.minimum.y, middle.z};
            target.radius = std::max(.001f, glm::length(Vec2{definition.maximum.x - middle.x,
                                                             definition.maximum.z - middle.z}));
            target.height = std::max(.001f, definition.maximum.y - definition.minimum.y);
            target.surface = m_surfaces[i];
        }
        result.push_back(target);
    }
    return result;
}

bool LegendsScenery::strike(s32 target, f32 damage, u32 flags) {
    if (target < kTargetBase || static_cast<usize>(target - kTargetBase) >= m_health.size() ||
        !std::isfinite(damage) || damage < 1 || (flags & damage::kMagic) != 0 ||
        (flags & damage::kGas) != 0) {
        return false;
    }
    auto& health = m_health[static_cast<usize>(target - kTargetBase)];
    if (health <= 0) {
        return false;
    }
    health -= static_cast<s32>(std::min(damage, static_cast<f32>(health)));
    // Native SWAL contact (e0077738) sets flag0x40000 on a surviving wall;
    // e001ea34 selects the white combiner for that flag. Keep the flash on
    // the simulation clock, like the shared game's one-tick wall hit flash.
    m_flash[static_cast<usize>(target - kTargetBase)] = health > 0 ? 1.0f / 30 : 0;
    return true;
}

void LegendsScenery::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0)
        return;
    for (auto& flash : m_flash)
        flash = std::max(0.0f, flash - seconds);
}

f32 LegendsScenery::whiteBlend(usize object) const {
    for (usize i = 0; i < m_definitions.size(); ++i) {
        if (m_definitions[i].object == object)
            return m_flash[i] > 0 ? 1.0f : 0.0f;
    }
    return 0;
}

bool LegendsScenery::visible(usize object) const {
    for (usize i = 0; i < m_definitions.size(); ++i) {
        if (m_definitions[i].object == object) {
            return m_health[i] > 0;
        }
    }
    return true;
}

void LegendsScenery::syncCollision(WorldCollision& collision) const {
    for (usize i = 0; i < m_definitions.size(); ++i) {
        collision.setSolid(static_cast<s32>(m_definitions[i].object), m_health[i] > 0);
    }
}

s32 LegendsScenery::health(usize slot) const {
    return slot < m_health.size() ? m_health[slot] : 0;
}
} // namespace gdl::game
