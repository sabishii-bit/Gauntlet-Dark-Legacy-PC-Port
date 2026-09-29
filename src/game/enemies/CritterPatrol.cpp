#include "game/enemies/CritterPatrol.h"

#include "engine/core/Types.h"

namespace gdl::game {
bool CritterPatrol::chained(const LookoutRoute& route, usize index) {
    const s32 next = route.next[index];
    return next >= 0 && static_cast<usize>(next) != index;
}

void CritterPatrol::start(const LookoutRoute* route, const Vec3& position, f32 sight) {
    m_route = route;
    m_lookout = -1;
    m_sight = sight > 0.0f ? sight : 0.0f;
    if (route == nullptr) {
        return;
    }
    f32 nearest = kStartReach;
    for (usize i = 0; i < route->points.size() && i < route->next.size(); ++i) {
        if (!chained(*route, i)) {
            continue;
        }
        const f32 distance = glm::distance(route->points[i], position);
        if (distance < nearest) {
            nearest = distance;
            m_lookout = static_cast<s32>(i);
        }
    }
}

void CritterPatrol::end() {
    m_lookout = -1;
}

std::optional<Vec3> CritterPatrol::aim(const Vec3& position) {
    // NextWaypoint: the one it names, none when that is itself or nothing.
    while (m_route != nullptr && m_lookout >= 0 &&
           static_cast<usize>(m_lookout) < m_route->points.size()) {
        const auto at = static_cast<usize>(m_lookout);
        const Vec3& point = m_route->points[at];
        const Vec3 away = point - position;
        if (glm::dot(away, away) >= kReached * kReached) {
            return point;
        }
        const s32 next = at < m_route->next.size() ? m_route->next[at] : -1;
        m_lookout = next >= 0 && next != m_lookout ? next : -1;
    }
    m_lookout = -1;
    return std::nullopt;
}
} // namespace gdl::game
