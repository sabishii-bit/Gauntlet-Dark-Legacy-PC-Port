#pragma once

#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {
/** A priority-arbitrated horizontal shake of the attention, eye, or both. */
class CameraShake {
public:
    enum class Target : u8 { Attention, Eye, Both };
    void start(Target target = Target::Attention, s32 lead = 0, s32 ticks = 90, f32 radius = 0.1f,
               s32 priority = 100);
    void clear();
    void update(s32 ticks);
    bool active() const { return m_ticks >= 0; }
    Vec3 offset() const;
    WorldCamera apply(const WorldCamera& camera, const Vec3& attention) const;

private:
    s32 m_ticks = -1;
    s32 m_lead = 0;
    s32 m_priority = 0;
    f32 m_radius = 0.1f;
    Target m_target = Target::Attention;
};
} // namespace gdl::game
