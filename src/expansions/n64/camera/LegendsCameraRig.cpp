#include "expansions/n64/camera/LegendsCameraRig.h"

#include <cmath>

#include "engine/math/PortableMath.h"

namespace gdl::game {
namespace {
/** The length in the native order of sums, ((x^2 + y^2) + z^2). */
f32 length(const Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

f32 level(const Vec3& value) {
    return std::sqrt(value.x * value.x + value.z * value.z);
}
} // namespace

void LegendsCameraRig::setTimes(const Times& times) {
    m_times = times;
    m_rotation.plan(times);
    m_attention.plan(times);
    m_centre.plan(times);
    m_orbit.plan(times);
    m_distance.plan(times);
}

void LegendsCameraRig::settle(const WorldCamera& view) {
    m_orbit.turnOff();
    m_distance.turnOff();
    m_attention.turnOff();
    setRotation(Vec3{view.pitch, view.yaw, view.roll}, true);
    setCentre(view.position, true);
}

void LegendsCameraRig::setCentre(const Vec3& point, bool cut) {
    m_centre.set(point, cut, m_times);
}

void LegendsCameraRig::setOrbit(f32 distance, f32 pitch, f32 yaw, bool cut) {
    m_orbit.set({-pitch, yaw, 0}, cut, m_times);
    m_distance.set({0, 0, distance}, cut, m_times);
}

void LegendsCameraRig::setAttention(const std::optional<Vec3>& point, bool cut) {
    if (point) {
        m_attention.set(*point, cut, m_times);
    } else {
        m_attention.turnOff();
    }
}

void LegendsCameraRig::setRotation(const std::optional<Vec3>& angles, bool cut) {
    if (angles) {
        m_rotation.set({-angles->x, angles->y, -angles->z}, cut, m_times);
    } else {
        m_rotation.turnOff();
    }
}

void LegendsCameraRig::returnTo(const Vec3& eye, f32 pitch, f32 yaw) {
    // e007df20 cuts the rotation to the angles the view shows (e0014fe4 of e007db00's build,
    // its roll dropped) as the attention goes off, then turns it toward the return's.
    const WorldCamera shown = build();
    m_attention.turnOff();
    setRotation(Vec3{shown.pitch, shown.yaw, 0}, true);
    setRotation(Vec3{pitch, yaw, 0}, false);
    // The distance and the orbit's angles back along the offset put the eye at `eye`: the orbit
    // looks from there, half a turn round from the offset's bearing.
    const Vec3 offset = eye - m_centre.goal();
    m_distance.set({0, 0, length(offset)}, false, m_times);
    const f32 bearing = portable::atan2(offset.x, offset.z);
    const f32 rise = portable::atan2(offset.y, level(offset));
    m_orbit.set({-rise, bearing - kPi, 0}, false, m_times);
}

bool LegendsCameraRig::step(f32 seconds) {
    // e007dd70's order; every channel advances whatever the others report.
    bool still = m_centre.advance(seconds);
    still = m_attention.advance(seconds) && still;
    still = m_rotation.advance(seconds) && still;
    still = m_orbit.advance(seconds) && still;
    still = m_distance.advance(seconds) && still;
    m_view = build();
    return still;
}

std::optional<Vec3> LegendsCameraRig::attention() const {
    return m_attention.on() ? std::optional<Vec3>{m_attention.value()} : std::nullopt;
}

WorldCamera LegendsCameraRig::build() const {
    // e007db00 starts from e00a929c's identity at the centre.
    WorldCamera view;
    view.position = m_centre.value();
    if (m_orbit.on() && m_distance.on()) {
        // e00141f0 carries (0, 0, -distance) through e0014d04's rows: back along the forward.
        WorldCamera orbit;
        orbit.pitch = -m_orbit.value().x;
        orbit.yaw = m_orbit.value().y;
        view.position -= orbit.forward() * m_distance.value().z;
    }
    if (m_attention.on()) {
        // A look from the eye at the point, e0010e50's arc tangents of the offset. With the
        // rotation on as well e007db00 turns this by it (e0013fc8), which no Tower script
        // leaves for a step; the look is kept alone, as a zero rotation leaves it.
        const Vec3 look = m_attention.value() - view.position;
        view.yaw = portable::atan2(look.x, look.z);
        view.pitch = -portable::atan2(look.y, level(look));
    } else if (m_rotation.on()) {
        const Vec3& turn = m_rotation.value();
        view.pitch = -turn.x;
        view.yaw = turn.y;
        view.roll = -turn.z;
    }
    return view;
}

} // namespace gdl::game
