#include "game/world/TowerCamera.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace gdl::game {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

float wrapAngle(float angle) {
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
    for (int k = 0; k < 3; ++k) {
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
    std::int32_t still = 0;
    for (std::int32_t i = 0; i < kSettleLimit && still < kRing; ++i) {
        still = update(subjects, markers, range, view, 1.0f / kStepRate) ? 0 : still + 1;
    }
}

bool TowerCamera::update(std::span<const CameraSubject> subjects,
                         std::span<const WorldLocator> markers, const CameraRange& range,
                         const CameraView& view, float seconds) {
    if (subjects.empty()) {
        return true;
    }
    const Vec3 middle = middleOf(subjects, range);
    m_ringIndex = (m_ringIndex + 1) % kRing;
    m_ringPositions[static_cast<std::size_t>(m_ringIndex)] = middle;

    chooseMarker(markers, middle);
    aim(markers, subjects.size(), range, false);
    const bool turned = turn(seconds * kStepRate);
    const bool followed = follow();

    float current = range.radiusMin;
    if (range.radiusMin < range.radiusMax) {
        if (subjects.size() == 1) {
            current = range.radiusMin * kAloneDistance;
        } else if (m_marker >= 0 && static_cast<std::size_t>(m_marker) < markers.size() &&
                   markers[static_cast<std::size_t>(m_marker)].delay != 0) {
            current = static_cast<float>(markers[static_cast<std::size_t>(m_marker)].delay);
        } else {
            current = range.radiusMax * kPartyDistance;
        }
    }
    m_ringDistances[static_cast<std::size_t>(m_ringIndex)] = fitDistance(
        m_ringPositions[static_cast<std::size_t>(m_ringIndex)], subjects, view, range, current);
    float slack = 0.0f;
    for (const float wanted : m_ringDistances) {
        slack += wanted - m_distance;
    }
    slack /= static_cast<float>(kRing);
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
    std::int32_t best = -1;
    float bestDistance = 0.0f;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        if (static_cast<std::int32_t>(i) == m_marker) {
            continue;
        }
        const Vec3 away = near - markers[i].position;
        const float distance = glm::dot(away, away);
        if (best < 0 || distance < bestDistance) {
            best = static_cast<std::int32_t>(i);
            bestDistance = distance;
        }
    }
    if (m_marker < 0 || static_cast<std::size_t>(m_marker) >= markers.size()) {
        m_marker = best;
        return;
    }
    const Vec3 away = near - markers[static_cast<std::size_t>(m_marker)].position;
    if (best >= 0 && bestDistance <= kSwitchRatio * glm::dot(away, away)) {
        m_marker = best;
    }
}

/** Points the camera at the marker's angles: at once with `jump`, else by starting a turn
 * whenever the marker has changed. */
void TowerCamera::aim(std::span<const WorldLocator> markers, std::size_t count,
                      const CameraRange& range, bool jump) {
    float yawTarget = 0.0f;
    float pitchTarget = 0.0f;
    if (m_marker >= 0 && static_cast<std::size_t>(m_marker) < markers.size()) {
        yawTarget = markerYaw(markers[static_cast<std::size_t>(m_marker)]);
        pitchTarget = markerPitch(markers[static_cast<std::size_t>(m_marker)]);
    }
    if (count > 1) {
        pitchTarget = std::max(pitchTarget, range.minPitch);
    }
    if (jump) {
        m_camera.yaw = yawTarget;
        m_camera.pitch = pitchTarget;
        m_aimedMarker = m_marker;
        m_turnStep = static_cast<float>(kTurnSteps);
        return;
    }
    if (m_marker == m_aimedMarker) {
        return;
    }
    m_aimedMarker = m_marker;
    m_yawRate = wrapAngle(yawTarget - m_camera.yaw) / static_cast<float>(kTurnSteps);
    m_pitchRate = wrapAngle(pitchTarget - m_camera.pitch) / static_cast<float>(kTurnSteps);
    m_turnStep = 0.0f;
}

bool TowerCamera::turn(float steps) {
    if (!turning()) {
        return false;
    }
    const float taken = std::min(steps, static_cast<float>(kTurnSteps) - m_turnStep);
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
    pull /= static_cast<float>(kRing);
    m_attention += pull;
    return pull != Vec3{0.0f, 0.0f, 0.0f};
}

/**
 * How far behind `point` the camera must sit so that every player, standing and at the
 * followed point, fits inside the view; with some slack so the distance does not chatter.
 */
float TowerCamera::fitDistance(const Vec3& point, std::span<const CameraSubject> subjects,
                               const CameraView& view, const CameraRange& range,
                               float current) const {
    if (range.radiusMin >= range.radiusMax || subjects.size() <= 1) {
        return range.radiusMin;
    }
    const Vec3 forward = m_camera.forward();
    const Vec3 right = m_camera.right();
    const Vec3 up = m_camera.up();
    const float tanX = std::tan(view.horizontalFov * 0.5f);
    const float tanY = tanX / view.aspect;
    float required = 0.0f;
    for (const CameraSubject& subject : subjects) {
        for (const Vec3& spot : {subject.follow, subject.feet}) {
            const Vec3 relative = spot - point;
            const float x = glm::dot(relative, right);
            const float y = glm::dot(relative, up);
            const float z = glm::dot(relative, forward);
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
