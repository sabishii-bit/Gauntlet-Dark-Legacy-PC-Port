#include "expansions/n64/level/LegendsBlast.h"

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

LegendsBlast::LegendsBlast(LegendsFixtureEvent event, f32 damageScale) : m_event(event) {
    if (!finite(m_event.position) || !std::isfinite(m_event.radius) || m_event.radius <= 0 ||
        m_event.radius > 64 || !std::isfinite(m_event.damage) || m_event.damage < 0 ||
        !std::isfinite(damageScale) || damageScale < 0 ||
        !std::isfinite(m_event.damage * damageScale) || !std::isfinite(m_event.seconds) ||
        m_event.seconds <= 0 || m_event.seconds > 300 || !std::isfinite(m_event.growFraction) ||
        m_event.growFraction < 0 || !std::isfinite(m_event.shrinkFraction) ||
        m_event.shrinkFraction < 0 || m_event.growFraction + m_event.shrinkFraction > 1) {
        throw std::invalid_argument("Invalid native Legends blast");
    }
    m_event.damage *= damageScale;
    // e007496c/e0074eec round the physical/elemental halves separately. Only
    // the physical portion attenuates in e0077b84; poison disables falloff.
    const bool poison = m_event.kind == LegendsFixtureEvent::Kind::Poison;
    m_physical = std::floor(m_event.damage * (poison ? .1f : .66f) + .5f);
    m_elemental = std::floor(m_event.damage * (poison ? .9f : .33f) + .5f);
}

std::optional<LegendsBlast::Area> LegendsBlast::step(f32 seconds) {
    if (!std::isfinite(seconds) || seconds < 0) {
        throw std::invalid_argument("Invalid native Legends blast time step");
    }
    m_elapsed = std::min(m_elapsed + seconds, static_cast<f64>(m_event.seconds));
    if (!alive()) {
        m_radius = 0;
        return std::nullopt;
    }
    const f32 progress = static_cast<f32>(m_elapsed / m_event.seconds);
    f32 scale = 1;
    if (m_event.growFraction > 0 && progress < m_event.growFraction) {
        scale = progress / m_event.growFraction;
    } else if (m_event.shrinkFraction > 0 && progress > 1 - m_event.shrinkFraction) {
        scale = (1 - progress) / m_event.shrinkFraction;
    }
    m_radius = m_event.radius * scale;
    return Area{m_event.position, m_radius, m_physical + m_elemental};
}

f32 LegendsBlast::damageAt(const Vec3& position) const {
    if (!finite(position)) {
        return 0;
    }
    return physicalDamageAt(position) + m_elemental;
}

f32 LegendsBlast::physicalDamageAt(const Vec3& position) const {
    if (!finite(position)) {
        return 0;
    }
    if (m_event.noFalloff) {
        return m_physical;
    }
    const Vec3 delta = position - m_event.position;
    const f32 fraction =
        std::clamp(glm::dot(delta, delta) / (m_event.radius * m_event.radius), 0.0f, .95f);
    return std::floor(m_physical * (1 - fraction) + .9f);
}

std::optional<f32> LegendsBlast::touch(const LegendsPortalVisitor& visitor) {
    if (!alive() || m_radius <= 0 || !visitor.alive || visitor.player >= 4 ||
        (m_players & (1U << visitor.player)) != 0 || !finite(visitor.position) ||
        !std::isfinite(visitor.radius) || visitor.radius < 0 || visitor.radius > 64 ||
        !std::isfinite(visitor.height) || visitor.height <= 0 || visitor.height > 128) {
        return std::nullopt;
    }
    // Use the imported actor's ordinary GDL body extent, rather than create a
    // parallel native player collider. The native hit mask survives the blast.
    Vec3 closest = visitor.position;
    closest.y =
        std::clamp(m_event.position.y, visitor.position.y, visitor.position.y + visitor.height);
    const Vec3 delta = closest - m_event.position;
    const f32 reach = m_radius + visitor.radius;
    if (glm::dot(delta, delta) > reach * reach) {
        return std::nullopt;
    }
    m_players |= static_cast<u8>(1U << visitor.player);
    return damageAt(visitor.position + Vec3{0, visitor.height * .5f, 0});
}

} // namespace gdl::game
