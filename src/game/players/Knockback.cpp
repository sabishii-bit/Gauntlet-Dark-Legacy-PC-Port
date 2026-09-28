#include "game/players/Knockback.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gdl::game {

void Knockback::queue(const Vec3& push, u32 flags, f32 damage) {
    m_force += push;
    m_flags |= flags;
    m_damage += std::max(damage, 0.0f);
}

std::optional<f32> Knockback::kick(f32 facing, bool pojo) {
    m_force.y = std::min(m_force.y, 0.0f); // nothing is pushed up
    const u32 flags = m_flags;
    const Vec3 force{m_force.x, 0.0f, m_force.z};
    const f32 damage = m_damage;
    m_force = Vec3{0.0f};
    m_flags = 0;
    m_damage = 0.0f;
    m_fast = (flags & kFastSlide) != 0;
    if (damage <= kKickFrom) {
        return std::nullopt;
    }
    f32 strength = 0.0f;
    if ((flags & kWhirlwind) != 0) {
        strength = kWhirlKick;
    } else if ((flags & kBlownAway) != 0) {
        strength = kBlownKick;
    } else if ((flags & (kKnockDown | kKnockOver)) != 0) {
        strength = pojo ? kPojoFallKick : kFallKick;
    } else if ((flags & kKnockBack) != 0) {
        strength = kKnockKick;
    } else {
        return std::nullopt;
    }
    m_velocity += force * strength;
    if (glm::length(force) <= 0.0f) {
        return std::nullopt;
    }
    // Facing along the push, or against it: whichever is the nearer turn.
    constexpr f32 kPi = std::numbers::pi_v<f32>;
    f32 heading = std::atan2(force.x, force.z);
    if (std::abs(std::remainder(heading - facing, 2.0f * kPi)) > 0.5f * kPi) {
        heading = std::remainder(heading + kPi, 2.0f * kPi);
    }
    return heading;
}

Vec3 Knockback::step(f32 seconds, f32 pace) {
    if (seconds <= 0.0f) {
        return Vec3{0.0f};
    }
    const f32 limit = (m_fast ? kFastLimit : kPaceLimit * pace) * seconds;
    const Vec3 travel{std::clamp(m_velocity.x * seconds, -limit, limit), 0.0f,
                      std::clamp(m_velocity.z * seconds, -limit, limit)};
    m_fast = false;
    m_velocity *= std::pow(kDecay, seconds * kFrameRate);
    if (!sliding()) {
        m_velocity = Vec3{0.0f};
    }
    return travel;
}

void Knockback::clear() {
    *this = Knockback{};
}

} // namespace gdl::game
