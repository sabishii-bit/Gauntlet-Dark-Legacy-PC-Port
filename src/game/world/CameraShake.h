#pragma once

#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {
/** SFXX shake: a 90-tick horizontal orbit of the attention point, not camera drift. */
class CameraShake {
public:
    void start() { m_ticks = 90; }
    void clear() { m_ticks = -1; }
    void update(s32 ticks);
    bool active() const { return m_ticks >= 0; }
    Vec3 offset() const;
    WorldCamera apply(const WorldCamera& camera, const Vec3& attention) const;

private:
    s32 m_ticks = -1;
};
} // namespace gdl::game
