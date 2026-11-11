#include "expansions/n64/camera/LegendsCamera.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "engine/core/Error.h"

namespace gdl::game {
namespace {
constexpr f32 kStep = 1.0f / 30.0f;
constexpr f32 kEntryAcceleration = .5f;
constexpr f32 kEntryCruise = 1.0f;
constexpr f32 kEntryBraking = .2f;

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           glm::all(glm::lessThanEqual(glm::abs(value), Vec3{100000}));
}

f32 angle(f32 value) {
    return std::remainder(value, 2 * kPi);
}

// Native e00363e0 accelerates to a per-update angular cap, then brakes in the
// last four cap-widths. It takes the short route across the signed angle seam.
f32 turn(f32 current, f32 desired, f32 cap, f32& velocity) {
    const f32 difference = angle(desired - current);
    const f32 distance = std::abs(difference);
    if (distance > cap * 4) {
        velocity = std::min(velocity + cap / 16, cap);
    } else if (distance > cap) {
        velocity = std::max(velocity - cap / 4, cap / 4);
    } else if (distance > cap / 4) {
        velocity = cap / 4;
    } else {
        return angle(desired);
    }
    return angle(current + std::copysign(velocity, difference));
}

s32 nearestMarker(const formats::LegendsN64CameraData& data, const Vec3& point, s32 previous) {
    // e0035018 compares squared distances in integer XYZ quarters; marker zero
    // is reserved for the opening view. Retain the old marker until another is
    // at least 1.5 times closer, preventing oscillation at Voronoi boundaries.
    const auto distance = [&](usize index) {
        const Vec3 raw = data.markers[index].position * 4.0f;
        const Vec3 delta{std::trunc(point.x * 4) - raw.x, std::trunc(point.y * 4) - raw.y,
                         std::trunc(point.z * 4) - raw.z};
        return glm::dot(delta, delta);
    };
    s32 best = 1;
    f32 bestDistance = distance(1);
    for (usize i = 2; i < data.markers.size(); ++i) {
        const f32 candidate = distance(i);
        if (candidate < bestDistance) {
            best = static_cast<s32>(i);
            bestDistance = candidate;
        }
    }
    if (previous > 0 && static_cast<usize>(previous) < data.markers.size() &&
        distance(static_cast<usize>(previous)) * 4 < bestDistance * 9) {
        return previous;
    }
    return best;
}
} // namespace

void LegendsCamera::EntryTrack::retarget(Vec3 destination) {
    goal = destination;
    if (angular) {
        for (s32 axis = 0; axis < 3; ++axis) {
            goal[axis] = angle(goal[axis]);
            value[axis] = angle(value[axis]);
        }
    }
    remaining = goal - value;
    if (angular) {
        for (s32 axis = 0; axis < 3; ++axis) {
            remaining[axis] = angle(remaining[axis]);
        }
    }
    elapsed = 0;
    const f32 length = glm::length(remaining);
    if (length <= .001f) {
        value = goal;
        speed = 0;
        phase = 3;
        return;
    }
    peak = length / ((kEntryAcceleration + kEntryBraking) * .5f + kEntryCruise);
    acceleration = (peak - speed) / kEntryAcceleration;
    phase = 0;
}

void LegendsCamera::EntryTrack::advance(f32 seconds) {
    if (done()) {
        return;
    }
    if (glm::all(glm::lessThan(glm::abs(remaining), Vec3{.001f}))) {
        goal = value;
        speed = 0;
        phase = 3;
        return;
    }
    f32 distance = 0;
    // The native phase boundary is strictly greater: a step exactly on the
    // endpoint completes on the following update. Integrate a crossing step's
    // remainder in the next phase rather than quantizing the authored timing.
    if (phase == 0) {
        const f32 slice = std::min(seconds, kEntryAcceleration - elapsed);
        distance += (speed + acceleration * slice * .5f) * slice;
        speed += acceleration * slice;
        if (seconds <= kEntryAcceleration - elapsed) {
            elapsed += slice;
            seconds = 0;
        } else {
            seconds -= slice;
            elapsed = 0;
            phase = 1;
        }
    }
    if (phase == 1) {
        const f32 slice = std::min(seconds, kEntryCruise - elapsed);
        speed = peak;
        distance += speed * slice;
        if (seconds <= kEntryCruise - elapsed) {
            elapsed += slice;
            seconds = 0;
        } else {
            seconds -= slice;
            elapsed = 0;
            phase = 2;
        }
    }
    if (phase == 2) {
        const f32 slice = std::min(seconds, kEntryBraking - elapsed);
        const f32 braking = -peak / kEntryBraking;
        distance += (speed + braking * slice * .5f) * slice;
        speed += braking * slice;
        if (seconds <= kEntryBraking - elapsed) {
            elapsed += slice;
        } else {
            elapsed = 0;
            phase = 3;
        }
    }
    if (done()) {
        value = goal;
        speed = 0;
    } else if (distance > 0) {
        const Vec3 step = remaining * (distance / glm::length(remaining));
        value += step;
        remaining -= step;
        if (angular) {
            for (s32 axis = 0; axis < 3; ++axis) {
                value[axis] = angle(value[axis]);
            }
        }
    }
}

void LegendsCamera::reset(formats::LegendsN64CameraData data, const Vec3& feet, bool opening) {
    const std::array party{CameraSubject{feet, feet, 0}};
    resetImpl(std::move(data), party, opening);
}

void LegendsCamera::reset(formats::LegendsN64CameraData data, std::span<const CameraSubject> party,
                          bool opening) {
    resetImpl(std::move(data), party, opening);
}

void LegendsCamera::resetImpl(formats::LegendsN64CameraData data,
                              std::span<const CameraSubject> party, bool opening) {
    if (party.empty() || party.size() > m_party.size()) {
        throw FormatError("Legends camera: invalid party size");
    }
    Vec3 feet{0};
    for (const auto& subject : party) {
        if (!finite(subject.feet) || !std::isfinite(subject.viewRadius) || subject.viewRadius < 0 ||
            subject.viewRadius > 1000) {
            throw FormatError("Legends camera: invalid party subject");
        }
        feet += subject.feet;
    }
    feet /= static_cast<f32>(party.size());
    if (!finite(feet) || data.markers.size() < 2 || data.markers.size() > 4096 ||
        (data.tower && data.towerExits.empty()) ||
        (data.bossFocus && (!finite(*data.bossFocus) || data.tower))) {
        throw FormatError("Legends camera: missing native camera data or invalid actor position");
    }
    for (const auto& marker : data.markers) {
        if (!finite(marker.position) || !std::isfinite(marker.pitch) ||
            !std::isfinite(marker.yaw)) {
            throw FormatError("Legends camera: invalid marker");
        }
    }
    for (const auto& exit : data.towerExits) {
        if (!finite(exit) || exit.x * exit.x + exit.z * exit.z < .0001f) {
            throw FormatError("Legends camera: invalid tower return direction");
        }
    }
    *this = {};
    m_data = std::move(data);
    m_bossArena = m_data.bossFocus.has_value();
    if (opening && m_bossArena) {
        // e0037824 clears the boss-focus enable; e006b110 supplies only a
        // provisional position. e08842e8 enables the grounded actor's focus
        // when it wakes. Do not pull the opening camera down toward that
        // provisional position while the boss is still in INIT.
        m_data.bossFocus.reset();
    }
    m_partySize = party.size();
    std::ranges::copy(party, m_party.begin());
    m_mode = m_data.tower ? Mode::TowerInner : Mode::Markers;
    m_marker = nearestMarker(m_data, feet, -1);
    const auto& marker = m_data.markers[static_cast<usize>(m_marker)];
    m_camera.yaw = marker.yaw;
    m_camera.pitch = marker.pitch;
    m_smoothed = partyTarget(feet) + m_camera.up() * 3.0f;
    m_distance = m_bossArena ? 50.0f : 36.0f;
    if (!m_data.tower && opening) {
        // e00339a0 replaces marker0's angles with the nearest follow marker's
        // angles. Marker0 supplies the orbit centre only, with integer /4
        // truncation (unlike the quarter-unit follow-marker distance tests).
        m_introPosition.value = glm::trunc(m_data.markers.front().position);
        m_introPosition.retarget(feet);
        m_introFocus.value = feet;
        m_introFocus.retarget(feet);
        m_introAngles.value = {marker.pitch, marker.yaw, 0};
        m_introAngles.angular = true;
        m_introAngles.retarget(m_introAngles.value);
        m_introFocusOffset = m_camera.up() * 3.0f;
        m_previousTarget = feet;
        m_introActive = true;
        // e0037824 advances state1 three times before the first rendered frame.
        // Preserve that pre-roll rather than adding .36 seconds to each entry.
        for (s32 step = 0; step < 3 && m_introActive; ++step) {
            advanceIntro(feet, .12f);
        }
        m_previousCamera = m_camera;
        return;
    }
    f32 pitch = m_camera.pitch;
    f32 yaw = m_camera.yaw;
    m_attention = target(feet, pitch, yaw);
    m_camera.pitch = pitch;
    m_camera.yaw = yaw;
    m_previousTarget = m_attention;
    // e0869a6c: inner min55, middle/outer min40. e0034ac8's min50
    // override is for level ordinal8 (bosses), not realm8: tower2 is level1.
    m_distance =
        m_mode == Mode::TowerInner ? 55.0f : (m_data.tower ? 40.0f : (m_bossArena ? 50.0f : 36.0f));
    m_transition = false;
    place();
    m_previousCamera = m_camera;
}

void LegendsCamera::advanceIntro(const Vec3& feet, f32 seconds) {
    // e0038a90 updates the target without the ordinary follow low-pass filter.
    // Its orientation-derived focus offset remains fixed until the handoff.
    const Vec3 focus = feet + m_introFocusOffset;
    if (focus != m_previousTarget) {
        m_introPosition.retarget(focus);
        m_introFocus.retarget(focus);
        const s32 marker = nearestMarker(m_data, focus, m_marker);
        if (marker != m_marker) {
            m_marker = marker;
            const auto& pose = m_data.markers[static_cast<usize>(marker)];
            m_introAngles.retarget({pose.pitch, pose.yaw, 0});
        }
        m_previousTarget = focus;
    }
    m_introPosition.advance(seconds);
    m_introFocus.advance(seconds);
    m_introAngles.advance(seconds);
    WorldCamera orbit;
    orbit.pitch = m_introAngles.value.x;
    orbit.yaw = m_introAngles.value.y;
    m_camera.position = m_introPosition.value - orbit.forward() * m_distance;
    m_attention = m_introFocus.value;
    const Vec3 look = m_attention - m_camera.position;
    const f32 horizontal = std::hypot(look.x, look.z);
    // Native e007db00 looks at a separately animated target; simply lerping
    // yaw/pitch or orbiting marker0 misses the authored opening composition.
    if (horizontal > .0001f || std::abs(look.y) > .0001f) {
        m_camera.pitch = -std::atan2(look.y, horizontal);
        if (horizontal > .0001f) {
            m_camera.yaw = std::atan2(look.x, look.z);
        }
    }
    if (m_introPosition.done() && m_introFocus.done() && m_introAngles.done()) {
        m_introActive = false;
        m_camera.pitch = m_introAngles.goal.x;
        m_camera.yaw = m_introAngles.goal.y;
        m_attention = focus;
        m_smoothed = focus;
        m_previousTarget = focus;
        m_velocity = Vec3{0};
        place();
    }
}

Vec3 LegendsCamera::target(const Vec3& feet, f32& pitch, f32& yaw) {
    Vec3 desired = partyTarget(feet) + m_camera.up() * 3.0f;
    if (!m_data.tower) {
        // e00352d0's two-parts-history/one-part-target filter.
        m_smoothed = (m_smoothed * 2.0f + desired) / 3.0f;
        m_marker = nearestMarker(m_data, m_smoothed, m_marker);
        const auto& marker = m_data.markers[static_cast<usize>(m_marker)];
        pitch = marker.pitch;
        yaw = marker.yaw;
        return m_smoothed;
    }

    // e08697d0 uses complete 3D actor distance, not just XZ. In single-player
    // its party-separation gate is trivially satisfied. Keep both hysteresis
    // bands: exiting the outer zone at87 enters middle before inner at42.
    const f32 distance = glm::dot(feet, feet);
    const Mode previous = m_mode;
    if (distance >= 91 * 91) {
        m_mode = Mode::TowerOuter;
    } else if ((m_mode == Mode::TowerOuter && distance <= 87 * 87) ||
               (m_mode == Mode::TowerInner && distance >= 47 * 47)) {
        m_mode = Mode::TowerMiddle;
    } else if (distance <= 42 * 42) {
        m_mode = Mode::TowerInner;
    }
    if (previous != m_mode) {
        m_transition = true;
    }
    desired.y = std::max(desired.y, 1.0f); // e0035380's tower target floor
    if (m_mode == Mode::TowerInner) {
        pitch = .05f;
        // e0869a6c normalizes onto a radius20 horizontal ring at height7.
        // The exact origin is singular in retail; retain the last valid yaw.
        const f32 radial = std::hypot(desired.x, desired.z);
        yaw = radial > .0001f ? std::atan2(desired.x, desired.z) : m_camera.yaw;
        return {std::sin(yaw) * 20, 7, std::cos(yaw) * 20};
    }
    pitch = m_mode == Mode::TowerMiddle ? .1f : .7f;
    const f32 targetYaw = std::atan2(desired.x, desired.z);
    const Vec3* nearest = nullptr;
    f32 nearestAngle = std::numeric_limits<f32>::max();
    for (const auto& exit : m_data.towerExits) {
        // Native compares raw atan2 outputs here, without shortest-angle wrap.
        const f32 delta = std::atan2(exit.x, exit.z) - targetYaw;
        if (delta * delta < nearestAngle) {
            nearestAngle = delta * delta;
            nearest = &exit;
        }
    }
    const Vec3 ray = glm::normalize(Vec3{nearest->x, 0, nearest->z});
    const f32 projection = desired.x * ray.x + desired.z * ray.z;
    desired.x = projection * ray.x;
    desired.z = projection * ray.z;
    yaw = std::atan2(desired.x, desired.z);
    return desired;
}

void LegendsCamera::tick(const Vec3& feet) {
    m_previousCamera = m_camera;
    if (m_introActive) {
        advanceIntro(feet, kStep);
        return;
    }
    f32 desiredPitch = m_camera.pitch;
    f32 desiredYaw = m_camera.yaw;
    Vec3 desired = target(feet, desiredPitch, desiredYaw);
    const f32 angularCap = m_transition ? (m_data.tower ? .01f : .001f) : .05f;
    m_camera.pitch = turn(m_camera.pitch, desiredPitch, .02f, m_pitchVelocity);
    m_camera.yaw =
        turn(m_camera.yaw, desiredYaw, angularCap * (m_data.tower ? 1.5f : 1), m_yawVelocity);
    if (m_mode == Mode::TowerInner) {
        desired.x = std::sin(m_camera.yaw) * 20;
        desired.z = std::cos(m_camera.yaw) * 20;
    }
    if (angle(m_camera.yaw - desiredYaw) == 0 &&
        (m_mode != Mode::TowerInner || desired == m_attention)) {
        m_transition = false;
    }

    // e0036a2c is an acceleration-limited target follower. The native inner
    // tower mode bypasses it once a mode transition has settled (e086a0d8).
    const Vec3 step = desired - m_attention;
    const Vec3 difference = step - m_velocity;
    const f32 length = glm::length(difference);
    const f32 acceleration =
        m_data.tower ? (m_transition ? .1f : .3f) : (m_transition ? .05f : .1f);
    if ((m_mode == Mode::TowerInner && !m_transition) || length <= acceleration ||
        glm::dot(difference, m_velocity) < 0) {
        m_attention = desired;
        m_velocity = desired - m_previousTarget;
    } else {
        m_velocity += difference * (acceleration / length);
        m_attention += m_velocity;
    }
    m_previousTarget = desired;

    const f32 distance = followDistance();
    if (m_distance > distance) {
        m_distanceVelocity = std::max(m_distanceVelocity - .1f, -.95f);
        m_distance = std::max(distance, m_distance + m_distanceVelocity);
    } else if (m_distance < distance) {
        m_distanceVelocity = std::min(m_distanceVelocity + .1f, 1.05f);
        m_distance = std::min(distance, m_distance + m_distanceVelocity);
    } else {
        m_distanceVelocity = 0;
    }
    place();
}

void LegendsCamera::place() {
    m_camera.position = m_attention - m_camera.forward() * m_distance;
}

Vec3 LegendsCamera::partyFeet() const {
    Vec3 feet{0};
    for (usize i = 0; i < m_partySize; ++i) {
        feet += m_party[i].feet;
    }
    return feet / static_cast<f32>(m_partySize);
}

Vec3 LegendsCamera::partyTarget(const Vec3& feet) const {
    // e0035380 adds the arena focus as one additional subject, not half the
    // target regardless of player count. The common +3 camera-up offset is
    // added after averaging, exactly once.
    return m_data.bossFocus ? (feet * static_cast<f32>(m_partySize) + *m_data.bossFocus) /
                                  static_cast<f32>(m_partySize + 1)
                            : feet;
}

f32 LegendsCamera::followDistance() const {
    if (!m_bossArena) {
        return m_mode == Mode::TowerInner ? 55.0f : (m_data.tower ? 40.0f : 36.0f);
    }
    // e0034728/e0034ac8 fit every player's feet/head and the radius-eight boss
    // focus with a .95 outer margin and .7 inner hysteresis. Use the engine's
    // standard gameplay projection; video representation remains local.
    const CameraView projection;
    const f32 horizontal = std::tan(projection.horizontalFov * .5f) * .95f;
    const f32 vertical = horizontal / projection.aspect;
    const Vec3 eye = m_attention - m_camera.forward() * m_distance;
    bool outside = false;
    bool inside = true;
    const auto fit = [&](const Vec3& point, f32 radius) {
        const Vec3 relative = point - eye;
        const f32 depth = glm::dot(relative, m_camera.forward());
        const f32 x = std::abs(glm::dot(relative, m_camera.right())) + radius;
        const f32 y = std::abs(glm::dot(relative, m_camera.up())) + radius;
        outside = outside || depth <= 0 || x > depth * horizontal || y > depth * vertical;
        inside =
            inside && depth > 0 && x <= depth * horizontal * .7f && y <= depth * vertical * .7f;
    };
    f32 lowest = m_party.front().feet.y;
    f32 highest = lowest;
    for (usize i = 0; i < m_partySize; ++i) {
        const auto& subject = m_party[i];
        lowest = std::min(lowest, subject.feet.y);
        highest = std::max(highest, subject.feet.y);
        fit(subject.feet, subject.viewRadius);
        fit(subject.feet + Vec3{0, subject.viewRadius * 2, 0}, subject.viewRadius);
    }
    if (m_data.bossFocus) {
        fit(*m_data.bossFocus, m_bossRadius);
    }
    if (m_distance < 50) {
        return 50;
    }
    return outside ? 150 + highest - lowest : (inside ? 50 : m_distance);
}

void LegendsCamera::trackBoss(std::optional<Vec3> focus, f32 radius) {
    if (!m_bossArena || (focus && !finite(*focus)) || !std::isfinite(radius) || radius < 0 ||
        radius > 1000) {
        return;
    }
    m_data.bossFocus = focus;
    m_bossRadius = radius;
}

void LegendsCamera::update(const Vec3& feet, f32 seconds) {
    const std::array party{CameraSubject{feet, feet, 0}};
    update(party, seconds);
}

void LegendsCamera::update(std::span<const CameraSubject> party, f32 seconds) {
    if (party.empty() || party.size() > m_party.size()) {
        return;
    }
    Vec3 feet{0};
    for (const auto& subject : party) {
        if (!finite(subject.feet) || !std::isfinite(subject.viewRadius) || subject.viewRadius < 0 ||
            subject.viewRadius > 1000) {
            return;
        }
        feet += subject.feet;
    }
    feet /= static_cast<f32>(party.size());
    if (m_data.markers.empty() || !finite(feet) || !std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    m_partySize = party.size();
    std::ranges::copy(party, m_party.begin());
    m_accumulator += std::min(seconds, .25f);
    while (m_accumulator >= kStep) {
        tick(feet);
        m_accumulator -= kStep;
    }
}

WorldCamera LegendsCamera::sample(f32 extraSeconds) const {
    if (!std::isfinite(extraSeconds)) {
        extraSeconds = 0;
    }
    const f32 fraction =
        std::clamp((m_accumulator + std::max(extraSeconds, 0.0f)) / kStep, 0.0f, 1.0f);
    return m_camera.interpolate(m_previousCamera, fraction);
}

} // namespace gdl::game
