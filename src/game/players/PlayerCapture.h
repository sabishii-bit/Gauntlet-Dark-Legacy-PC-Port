#pragma once

#include <optional>

#include "engine/core/Types.h"

#include "game/players/PlayerActor.h"

namespace gdl::game {
/** External-node parenting and the following throw. Neither accepts walking input.
 * The held matrix is owned here, never a pointer into the captor's animation tree. */
class PlayerCapture {
public:
    void attach(s32 owner, bool boss, const Mat4& contact, PlayerActor& actor);
    void release(const Vec3& velocity, f32 damage);
    void clear();
    /** Returns deferred damage once, on landing. The motion driver applies health policy. */
    std::optional<f32> update(f32 seconds, PlayerActor& actor, const WorldCollision& collision);
    bool held() const { return m_body.has_value(); }
    bool flying() const { return m_flying; }
    bool active() const { return held() || flying(); }
    s32 owner() const { return m_owner; }
    bool boss() const { return m_boss; }
    const std::optional<Mat4>& body() const { return m_body; }

private:
    std::optional<Mat4> m_body;
    Vec3 m_velocity{0};
    f32 m_damage = 0;
    bool m_flying = false;
    s32 m_owner = -1;
    bool m_boss = false;
};
} // namespace gdl::game
