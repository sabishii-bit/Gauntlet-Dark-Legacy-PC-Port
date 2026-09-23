#include "game/world/TowerCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;

f32 wrapAngle(f32 angle) {
    while (angle > kPi) {
        angle -= 2.0f * kPi;
    }
    while (angle <= -kPi) {
        angle += 2.0f * kPi;
    }
    return angle;
}

} // namespace

/** The middle of the box around the followed points, kept inside the level's camera box. */
Vec3 TowerCamera::middleOf(std::span<const CameraSubject> subjects, const CameraRange& range) {
    Vec3 low{1.0e20f, 1.0e20f, 1.0e20f};
    Vec3 high{-1.0e20f, -1.0e20f, -1.0e20f};
    for (const CameraSubject& subject : subjects) {
        low = glm::min(low, subject.follow);
        high = glm::max(high, subject.follow);
    }
    Vec3 middle = subjects.empty() ? Vec3{0.0f, 0.0f, 0.0f} : (low + high) * 0.5f;
    for (s32 k = 0; k < 3; ++k) {
        if (range.boundsMin[k] < range.boundsMax[k]) {
            middle[k] = std::clamp(middle[k], range.boundsMin[k], range.boundsMax[k]);
        }
    }
    return middle;
}

void TowerCamera::reset(std::span<const CameraSubject> subjects,
                        std::span<const WorldLocator> markers, const CameraRange& range,
                        const CameraView& view) {
    m_camera = WorldCamera{};
    m_camera.yaw = kPi;
    m_ringPositions.fill(Vec3{0.0f, 0.0f, 0.0f});
    m_ringDistances.fill(0.0f);
    m_ringIndex = 0;
    m_marker = -1;
    m_aimedMarker = -1;
    m_attention = middleOf(subjects, range);
    chooseMarker(markers, m_attention);
    aim(markers, subjects.size(), range, true);
    m_distance = range.radiusMin;
    place();
    // A fresh start settles before anyone sees it: run until nothing moves for a whole ring.
    s32 still = 0;
    for (s32 i = 0; i < kSettleLimit && still < kRing; ++i) {
        still = update(subjects, markers, range, view, 1.0f / kStepRate) ? 0 : still + 1;
    }
}

bool TowerCamera::update(std::span<const CameraSubject> subjects,
                         std::span<const WorldLocator> markers, const CameraRange& range,
                         const CameraView& view, f32 seconds) {
    if (subjects.empty()) {
        return true;
    }
    const Vec3 middle = middleOf(subjects, range);
    m_ringIndex = (m_ringIndex + 1) % kRing;
    m_ringPositions[static_cast<usize>(m_ringIndex)] = middle;

    chooseMarker(markers, middle);
    aim(markers, subjects.size(), range, false);
    const bool turned = turn(seconds * kStepRate);
    const bool followed = follow();

    f32 current = range.radiusMin;
    if (range.radiusMin < range.radiusMax) {
        if (subjects.size() == 1) {
            current = range.radiusMin * kAloneDistance;
        } else if (m_marker >= 0 && static_cast<usize>(m_marker) < markers.size() &&
                   markers[static_cast<usize>(m_marker)].delay != 0) {
            current = static_cast<f32>(markers[static_cast<usize>(m_marker)].delay);
        } else {
            current = range.radiusMax * kPartyDistance;
        }
    }
    m_ringDistances[static_cast<usize>(m_ringIndex)] = fitDistance(
        m_ringPositions[static_cast<usize>(m_ringIndex)], subjects, view, range, current);
    f32 slack = 0.0f;
    for (const f32 wanted : m_ringDistances) {
        slack += wanted - m_distance;
    }
    slack /= static_cast<f32>(kRing);
    m_distance += slack;

    place();
    return turned || followed || slack != 0.0f;
}

/** The nearest marker takes over once it is clearly closer than the current one. */
void TowerCamera::chooseMarker(std::span<const WorldLocator> markers, const Vec3& near) {
    if (markers.empty()) {
        m_marker = -1;
        return;
    }
    s32 best = -1;
    f32 bestDistance = 0.0f;
    for (usize i = 0; i < markers.size(); ++i) {
        if (static_cast<s32>(i) == m_marker) {
            continue;
        }
        const Vec3 away = near - markers[i].position;
        const f32 distance = glm::dot(away, away);
        if (best < 0 || distance < bestDistance) {
            best = static_cast<s32>(i);
            bestDistance = distance;
        }
    }
    if (m_marker < 0 || static_cast<usize>(m_marker) >= markers.size()) {
        m_marker = best;
        return;
    }
    const Vec3 away = near - markers[static_cast<usize>(m_marker)].position;
    if (best >= 0 && bestDistance <= kSwitchRatio * glm::dot(away, away)) {
        m_marker = best;
    }
}

/** Points the camera at the marker's angles: at once with `jump`, else by starting a turn
 * whenever the marker has changed. */
void TowerCamera::aim(std::span<const WorldLocator> markers, usize count, const CameraRange& range,
                      bool jump) {
    f32 yawTarget = 0.0f;
    f32 pitchTarget = 0.0f;
    if (m_marker >= 0 && static_cast<usize>(m_marker) < markers.size()) {
        yawTarget = markerYaw(markers[static_cast<usize>(m_marker)]);
        pitchTarget = markerPitch(markers[static_cast<usize>(m_marker)]);
    }
    if (count > 1) {
        pitchTarget = std::max(pitchTarget, range.minPitch);
    }
    if (jump) {
        m_camera.yaw = yawTarget;
        m_camera.pitch = pitchTarget;
        m_aimedMarker = m_marker;
        m_turnStep = static_cast<f32>(kTurnSteps);
        return;
    }
    if (m_marker == m_aimedMarker) {
        return;
    }
    m_aimedMarker = m_marker;
    m_yawRate = wrapAngle(yawTarget - m_camera.yaw) / static_cast<f32>(kTurnSteps);
    m_pitchRate = wrapAngle(pitchTarget - m_camera.pitch) / static_cast<f32>(kTurnSteps);
    m_turnStep = 0.0f;
}

bool TowerCamera::turn(f32 steps) {
    if (!turning()) {
        return false;
    }
    const f32 taken = std::min(steps, static_cast<f32>(kTurnSteps) - m_turnStep);
    m_camera.yaw = wrapAngle(m_camera.yaw + m_yawRate * taken);
    m_camera.pitch = wrapAngle(m_camera.pitch + m_pitchRate * taken);
    m_turnStep += taken;
    return true;
}

/** Moves the look-at point by the average of where the last frames wanted it. */
bool TowerCamera::follow() {
    Vec3 pull{0.0f, 0.0f, 0.0f};
    for (const Vec3& wanted : m_ringPositions) {
        pull += wanted - m_attention;
    }
    pull /= static_cast<f32>(kRing);
    m_attention += pull;
    return pull != Vec3{0.0f, 0.0f, 0.0f};
}

/**
 * How far behind `point` the camera must sit so that every player, standing and at the
 * followed point, fits inside the view; with some slack so the distance does not chatter.
 */
f32 TowerCamera::fitDistance(const Vec3& point, std::span<const CameraSubject> subjects,
                             const CameraView& view, const CameraRange& range, f32 current) const {
    if (range.radiusMin >= range.radiusMax || subjects.size() <= 1) {
        return range.radiusMin;
    }
    const Vec3 forward = m_camera.forward();
    const Vec3 right = m_camera.right();
    const Vec3 up = m_camera.up();
    const f32 tanX = std::tan(view.horizontalFov * 0.5f);
    const f32 tanY = tanX / view.aspect;
    f32 required = 0.0f;
    for (const CameraSubject& subject : subjects) {
        for (const Vec3& spot : {subject.follow, subject.feet}) {
            const Vec3 relative = spot - point;
            const f32 x = glm::dot(relative, right);
            const f32 y = glm::dot(relative, up);
            const f32 z = glm::dot(relative, forward);
            required = std::max(required, std::max(std::abs(x) / tanX, std::abs(y) / tanY) - z);
        }
    }
    if (required <= range.radiusMin - kNearGap && range.radiusMin < current) {
        return range.radiusMin;
    }
    if (required <= current - kNearGap) {
        return required + kNearGap;
    }
    if (required <= current - kFarGap) {
        return current;
    }
    return required + kFarGap;
}

void TowerCamera::place() {
    m_camera.roll = 0.0f;
    m_camera.position = m_attention - m_camera.forward() * m_distance;
}

} // namespace gdl::game
