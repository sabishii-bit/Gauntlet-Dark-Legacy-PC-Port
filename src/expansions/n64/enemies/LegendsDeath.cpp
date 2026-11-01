#include "expansions/n64/enemies/LegendsDeath.h"

#include <cmath>

#include "game/enemies/EnemyKinds.h"

namespace gdl::game {

std::optional<LegendsDeath> LegendsDeath::decode(const formats::LegendsN64Fixture& fixture,
                                                 s32 players) {
    if (fixture.kind != formats::LegendsN64FixtureKind::Powerup ||
        (fixture.resolvedTemplateKey >> 8U) != 0x21 || fixture.sequence != "DETH" ||
        fixture.minimumPlayers > players || players < 1 || players > 4 ||
        !std::isfinite(glm::dot(fixture.position, fixture.position)))
        return {};
    LegendsDeath result;
    result.m_nativeIndex = fixture.nativeIndex;
    result.m_position = fixture.position;
    return result;
}

bool LegendsDeath::touch(const Vec3& feet, f32 radius, f32 height) {
    if (m_released || !std::isfinite(glm::dot(feet, feet)) || !std::isfinite(radius) ||
        !std::isfinite(height) || radius < 0 || height < 0 || feet.y > m_position.y + m_height ||
        feet.y + height < m_position.y)
        return false;
    const Vec2 delta{feet.x - m_position.x, feet.z - m_position.z};
    if (glm::dot(delta, delta) > (m_radius + radius) * (m_radius + radius))
        return false;
    // e0065618 returns6; e0048158/e00486f8 defer e0065560 until contact
    // resolution finishes. There is no pickup grant or extra proximity timer.
    m_pending = true;
    return true;
}

bool LegendsDeath::strike(f32 power) {
    if (m_released || !std::isfinite(power) || power <= 0)
        return false;
    // Imported GDL gameplay also wakes statues when struck, as CritterStatues
    // does. Keep that compatibility behavior distinct from the native touch.
    m_pending = true;
    return true;
}

EnemySpawn LegendsDeath::spawn() const {
    EnemySpawn result;
    result.kind = kDeathKind;
    result.tier = 1; // e0065560 -> e005ac34 class7, strength0 (red Death).
    result.position = m_position;
    result.placed = true;
    result.priority = EnemySpawn::Priority::Visible;
    return result;
}

ImportedTarget LegendsDeath::target(s32 index) const {
    return {kTargetBase + index, m_position, m_radius, m_height, !m_released, false};
}

} // namespace gdl::game
