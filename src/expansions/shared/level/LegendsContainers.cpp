#include "expansions/shared/level/LegendsContainers.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace gdl::game {
namespace {
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

LegendsContainer::LegendsContainer(LegendsContainerDefinition definition)
    : m_definition(std::move(definition)),
      m_health(std::isfinite(m_definition.damageThreshold)
                   ? static_cast<s32>(std::clamp(m_definition.damageThreshold, 0.0f, 32767.0f))
                   : 0) {}

bool LegendsContainer::targetable() const {
    return m_definition.interactive && m_state == State::Closed &&
           m_definition.kind == LegendsContainerDefinition::Kind::Barrel;
}

bool LegendsContainer::strike(f32 power) {
    // N64 e006cef4 compares each blow against PCon+2, with no stored hit-point sum.
    if (!targetable() || !std::isfinite(power) || power <= 0) {
        return false;
    }
    if (m_definition.damageMode == LegendsContainerDefinition::DamageMode::Accumulated) {
        // Dreamcast HitItem 8c0afaa0 stores remaining HP after armor, unlike
        // N64 PCon's per-blow threshold. Use the engine's bounded rounding.
        f32 felt = power - static_cast<f32>(m_definition.armor);
        if (felt <= 0) {
            felt = 1;
        }
        m_health = std::max(m_health - static_cast<s32>(std::lround(std::min(felt, 32767.0f))), 0);
        if (m_health > 0) {
            return true;
        }
    } else if (power < m_definition.damageThreshold) {
        return false;
    }
    m_state = State::Opening;
    return true;
}

bool LegendsContainer::touch(const Vec3& feet, f32 radius, f32 height, s32& keys) {
    if (!m_definition.interactive || m_state != State::Closed ||
        m_definition.kind != LegendsContainerDefinition::Kind::Chest || keys <= 0 ||
        !finite(feet) || !std::isfinite(radius) || radius < 0 || !std::isfinite(height) ||
        height < 0 || feet.y > m_definition.position.y + m_definition.closedObstacle.height ||
        feet.y + height < m_definition.position.y ||
        !m_definition.closedObstacle.touchedBy(
            {feet.x, m_definition.closedObstacle.centre.y, feet.z}, radius, .05f)) {
        return false;
    }
    // N64 e006d3d4 spends exactly one key on first contact, never on repeated ticks.
    --keys;
    m_state = State::Opening;
    return true;
}

bool LegendsContainer::finishOpening() {
    if (m_state != State::Opening) {
        return false;
    }
    m_state = State::Open;
    return true;
}

void LegendsContainer::collected() {
    if (m_state == State::Open) {
        m_state = State::Empty;
    }
}

void LegendsContainer::placeAt(const Vec3& feet) {
    if (!finite(feet)) {
        throw std::invalid_argument("Native container requires a finite placement");
    }
    const Vec3 delta = feet - m_definition.position;
    m_definition.position = feet;
    m_definition.closedObstacle.centre += delta;
}

Obstacle LegendsContainer::obstacle() const {
    auto result = m_definition.closedObstacle;
    // N64 e006cab8 retains and reindexes a chest's body while its opening tree
    // runs; e006d3d4 does not admit its contents before completion (0x1000).
    // Broken barrels, unlike opening chests, stop blocking immediately.
    result.solid =
        m_state == State::Closed ||
        (m_state == State::Opening && m_definition.kind == LegendsContainerDefinition::Kind::Chest);
    return result;
}

} // namespace gdl::game
