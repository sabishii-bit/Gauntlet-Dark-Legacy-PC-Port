#include "game/camera/TowerCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

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

TowerCamera::TowerCamera(const TowerCamera& other) {
    copyValues(other);
}
TowerCamera& TowerCamera::operator=(const TowerCamera& other) {
    if (this != &other) {
        auto binding = std::make_shared<const u8>(0);
        copyValues(other);
        m_checkpointBinding = std::move(binding);
    }
    return *this;
}
// A normal camera owns only scalar history. Moving its value need not alter
// the source, and must not transfer either camera's restore-domain identity.
TowerCamera::TowerCamera(TowerCamera&& other) noexcept
    // NOLINTNEXTLINE(performance-move-constructor-init): a moved camera leaves its source intact
    : TowerCamera(static_cast<const TowerCamera&>(other)) {}
TowerCamera& TowerCamera::operator=(TowerCamera&& other) noexcept {
    operator=(static_cast<const TowerCamera&>(other));
    return *this;
}
void TowerCamera::copyValues(const TowerCamera& other) noexcept {
    m_state = other.m_state;
}

bool TowerCamera::appendNativeDigest(NativeStateDigest& digest) const {
    // No other runtime values are owned here: subjects, markers, range and fit
    // projection are inputs supplied by the scene on each simulation update.
    const auto& [state, binding] = *this;
    (void)binding;
    return appendLocalDigest(digest, state);
}

bool TowerCamera::appendLocalDigest(NativeStateDigest& digest, const State& state) {
    digest.string("TowerCamera.native.1");
    digest.value(state);
    return digest.good();
}

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
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_state.camera = WorldCamera{};
    m_state.camera.yaw = kPi;
    m_state.ringPositions.fill(Vec3{0.0f, 0.0f, 0.0f});
    m_state.ringDistances.fill(0.0f);
    m_state.ringIndex = 0;
    m_state.marker = -1;
    m_state.aimedMarker = -1;
    m_state.attention = middleOf(subjects, range);
    chooseMarker(markers, m_state.attention);
    aim(markers, subjects.size(), range, true);
    m_state.distance = range.radiusMin;
    place();
    // A fresh start settles before anyone sees it: run until nothing moves for a whole ring.
    s32 still = 0;
    for (s32 i = 0; i < kSettleLimit && still < kRing; ++i) {
        still = update(subjects, markers, range, view, 1.0f / kStepRate) ? 0 : still + 1;
    }
}

void TowerCamera::snapAttention(std::span<const CameraSubject> subjects, const CameraRange& range) {
    if (subjects.empty()) {
        return;
    }
    m_state.attention = middleOf(subjects, range);
    m_state.ringPositions.fill(m_state.attention);
    place();
}

/** CamGetPlayerAvgPos(mode 5) centres screen bounds, including their view-space depth,
 * then unprojects. A world-space midpoint lets nearer players slip out of the view
 * when other party members move tangentially along its farther edges. */
Vec3 TowerCamera::projectedMiddleOf(std::span<const CameraSubject> subjects,
                                    const CameraRange& range) const {
    if (subjects.size() <= 1) {
        return middleOf(subjects, range);
    }
    Vec3 low{1.0e20f};
    Vec3 high{-1.0e20f};
    const Vec3 right = m_state.camera.right();
    const Vec3 up = m_state.camera.up();
    const Vec3 forward = m_state.camera.forward();
    for (const CameraSubject& subject : subjects) {
        const Vec3 relative = subject.follow - m_state.camera.position;
        const f32 depth = glm::dot(relative, forward);
        if (depth <= WorldCamera::kNear) {
            return middleOf(subjects, range);
        }
        // Viewport scale and centre cancel when unprojecting the bounds midpoint.
        const Vec3 projected{glm::dot(relative, right) / depth, glm::dot(relative, up) / depth,
                             depth};
        low = glm::min(low, projected);
        high = glm::max(high, projected);
    }
    const Vec3 centre = (low + high) * 0.5f;
    Vec3 middle = m_state.camera.position + right * (centre.x * centre.z) +
                  up * (centre.y * centre.z) + forward * centre.z;
    for (s32 k = 0; k < 3; ++k) {
        if (range.boundsMin[k] < range.boundsMax[k]) {
            middle[k] = std::clamp(middle[k], range.boundsMin[k], range.boundsMax[k]);
        }
    }
    return middle;
}

bool TowerCamera::update(std::span<const CameraSubject> subjects,
                         std::span<const WorldLocator> markers, const CameraRange& range,
                         const CameraView& view, f32 seconds) {
    if (subjects.empty()) {
        return true;
    }
    const Vec3 middle = middleOf(subjects, range);
    m_state.ringIndex = (m_state.ringIndex + 1) % kRing;
    m_state.ringPositions[static_cast<usize>(m_state.ringIndex)] =
        projectedMiddleOf(subjects, range);

    chooseMarker(markers, middle);
    aim(markers, subjects.size(), range, false);
    const bool turned = turn(seconds * kStepRate);
    const bool followed = follow();

    f32 current = range.radiusMin;
    if (range.radiusMin < range.radiusMax) {
        if (subjects.size() == 1) {
            current = range.radiusMin * kAloneDistance;
        } else if (m_state.marker >= 0 && static_cast<usize>(m_state.marker) < markers.size() &&
                   markers[static_cast<usize>(m_state.marker)].delay != 0) {
            current = static_cast<f32>(markers[static_cast<usize>(m_state.marker)].delay);
        } else {
            current = range.radiusMax * kPartyDistance;
        }
    }
    m_state.ringDistances[static_cast<usize>(m_state.ringIndex)] =
        fitDistance(m_state.ringPositions[static_cast<usize>(m_state.ringIndex)], subjects, view,
                    range, current);
    f32 slack = 0.0f;
    for (const f32 wanted : m_state.ringDistances) {
        slack += wanted - m_state.distance;
    }
    slack /= static_cast<f32>(kRing);
    m_state.distance += slack;

    place();
    return turned || followed || slack != 0.0f;
}

/** The nearest marker takes over once it is clearly closer than the current one. */
void TowerCamera::chooseMarker(std::span<const WorldLocator> markers, const Vec3& near) {
    if (markers.empty()) {
        m_state.marker = -1;
        return;
    }
    s32 best = -1;
    f32 bestDistance = 0.0f;
    for (usize i = 0; i < markers.size(); ++i) {
        if (static_cast<s32>(i) == m_state.marker) {
            continue;
        }
        const Vec3 away = near - markers[i].position;
        const f32 distance = glm::dot(away, away);
        if (best < 0 || distance < bestDistance) {
            best = static_cast<s32>(i);
            bestDistance = distance;
        }
    }
    if (m_state.marker < 0 || static_cast<usize>(m_state.marker) >= markers.size()) {
        m_state.marker = best;
        return;
    }
    const Vec3 away = near - markers[static_cast<usize>(m_state.marker)].position;
    if (best >= 0 && bestDistance <= kSwitchRatio * glm::dot(away, away)) {
        m_state.marker = best;
    }
}

/** Points the camera at the marker's angles: at once with `jump`, else by starting a turn
 * whenever the marker has changed. */
void TowerCamera::aim(std::span<const WorldLocator> markers, usize count, const CameraRange& range,
                      bool jump) {
    f32 yawTarget = 0.0f;
    f32 pitchTarget = 0.0f;
    if (m_state.marker >= 0 && static_cast<usize>(m_state.marker) < markers.size()) {
        yawTarget = markerYaw(markers[static_cast<usize>(m_state.marker)]);
        pitchTarget = markerPitch(markers[static_cast<usize>(m_state.marker)]);
    }
    if (count > 1) {
        pitchTarget = std::max(pitchTarget, range.minPitch);
    }
    if (jump) {
        m_state.camera.yaw = yawTarget;
        m_state.camera.pitch = pitchTarget;
        m_state.aimedMarker = m_state.marker;
        m_state.turnStep = static_cast<f32>(kTurnSteps);
        return;
    }
    if (m_state.marker == m_state.aimedMarker) {
        return;
    }
    m_state.aimedMarker = m_state.marker;
    m_state.yawRate = wrapAngle(yawTarget - m_state.camera.yaw) / static_cast<f32>(kTurnSteps);
    m_state.pitchRate =
        wrapAngle(pitchTarget - m_state.camera.pitch) / static_cast<f32>(kTurnSteps);
    m_state.turnStep = 0.0f;
}

bool TowerCamera::turn(f32 steps) {
    if (!turning()) {
        return false;
    }
    const f32 taken = std::min(steps, static_cast<f32>(kTurnSteps) - m_state.turnStep);
    m_state.camera.yaw = wrapAngle(m_state.camera.yaw + m_state.yawRate * taken);
    m_state.camera.pitch = wrapAngle(m_state.camera.pitch + m_state.pitchRate * taken);
    m_state.turnStep += taken;
    return true;
}

/** Moves the look-at point by the average of where the last frames wanted it. */
bool TowerCamera::follow() {
    Vec3 pull{0.0f, 0.0f, 0.0f};
    for (const Vec3& wanted : m_state.ringPositions) {
        pull += wanted - m_state.attention;
    }
    pull /= static_cast<f32>(kRing);
    m_state.attention += pull;
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
    const Vec3 forward = m_state.camera.forward();
    const Vec3 right = m_state.camera.right();
    const Vec3 up = m_state.camera.up();
    const f32 tanX = portable::tan(view.horizontalFov * 0.5f);
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
    return std::min(required + kFarGap, range.radiusMax);
}

void TowerCamera::place() {
    m_state.camera.roll = 0.0f;
    m_state.camera.position = m_state.attention - m_state.camera.forward() * m_state.distance;
}

} // namespace gdl::game
