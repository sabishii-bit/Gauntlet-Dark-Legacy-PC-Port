#include "game/players/PlayerCapture.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
void PlayerCapture::attach(s32 owner, bool boss, const Mat4& contact, PlayerActor& actor) {
    m_owner = owner;
    m_boss = boss;
    m_flying = false;
    m_damage = 0;
    m_velocity = Vec3{0};
    // PlayerSetParent subtracts the class collision-centre offset in node space.
    m_body = glm::translate(contact, -(actor.followPoint() - actor.position()));
    actor.place(Vec3{(*m_body)[3]});
}
void PlayerCapture::release(const Vec3& velocity, f32 damage) {
    if (!held()) {
        return;
    }
    m_body.reset();
    m_owner = -1;
    m_flying = true;
    m_velocity = velocity;
    m_damage = damage;
}
void PlayerCapture::clear() {
    *this = PlayerCapture{};
}
std::optional<f32> PlayerCapture::update(f32 seconds, PlayerActor& actor,
                                         const WorldCollision& collision) {
    if (!m_flying || seconds <= 0) {
        return std::nullopt;
    }
    // PlayerMotion clamps each horizontal component to 40 units/s for thrown
    // states, damps velocity by 0.9 per 30 Hz frame, and lowers toward the floor
    // at 16 units/s. It does not integrate the DAMG force as unbounded ballistics.
    constexpr f32 kLimit = 40;
    constexpr f32 kDescent = 16;
    const f32 maxStep = std::min(1.0f / 120.0f, std::max(actor.radius(), 0.1f) / (2 * kLimit));
    const s32 steps = static_cast<s32>(std::ceil(seconds / maxStep));
    const f32 dt = seconds / static_cast<f32>(steps);
    for (s32 step = 0; step < steps; ++step) {
        Vec3 to = actor.position() + Vec3{std::clamp(m_velocity.x, -kLimit, kLimit),
                                          std::clamp(m_velocity.y, 0.0f, kLimit) - kDescent,
                                          std::clamp(m_velocity.z, -kLimit, kLimit)} *
                                         dt;
        to = collision.resolveWalls(to, actor.radius(), to.y + PlayerActor::kFootClearance,
                                    to.y + actor.height());
        const auto floor = collision.floorAt(to, 0.2f + kDescent * dt, 0.2f);
        if (floor.has_value() && to.y <= floor->y + 0.2f) {
            to.y = floor->y;
            actor.place(to);
            const f32 damage = m_damage;
            clear();
            return damage;
        }
        actor.place(to);
        m_velocity *= std::pow(0.9f, dt * 30.0f);
    }
    return std::nullopt;
}
} // namespace gdl::game
