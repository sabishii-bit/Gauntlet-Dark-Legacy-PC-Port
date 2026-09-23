#include "game/world/BossCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;

Vec3 middleOf(std::span<const CameraSubject> party) {
    Vec3 sum{0.0f, 0.0f, 0.0f};
    for (const CameraSubject& subject : party) {
        sum += subject.follow;
    }
    return party.empty() ? sum : sum / static_cast<f32>(party.size());
}

f32 flatLength(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.z * v.z);
}

} // namespace

f32 BossCamera::wrapAngle(f32 angle) {
    while (angle > kPi) {
        angle -= 2.0f * kPi;
    }
    while (angle <= -kPi) {
        angle += 2.0f * kPi;
    }
    return angle;
}

/** Where the camera looks: the boss, offset by the record's near or far offset by how far
 * back the camera stands; the party's middle while the boss sleeps. */
Vec3 BossCamera::lookPoint(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                           const BossCameraInfo& record) const {
    if (!boss.awake) {
        return middleOf(party);
    }
    const f32 range = record.maxDistance - record.minDistance;
    const f32 t =
        range > 0.01f ? std::clamp((m_distance - record.minDistance) / range, 0.0f, 1.0f) : 1.0f;
    // BCAM bit 0 selects the live elevated anchor; otherwise retail uses the base
    // position initialized with the creature's geometry. Bit 4 tracks the party.
    Vec3 anchor = boss.baseAttention.value_or(boss.position);
    if ((record.flags & 0x10) != 0) {
        anchor = middleOf(party);
    } else if ((record.flags & 1) != 0) {
        anchor = boss.position + boss.attentionOffset;
    }
    return anchor + record.minAttention + (record.maxAttention - record.minAttention) * t;
}

/** The way the camera looks: along the party's line to the boss, or, when the party is
 * too far round the boss from where it faces, the boss's facing swung the record's most
 * toward them. */
f32 BossCamera::wantedYaw(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                          const BossCameraInfo& record) const {
    if (!boss.awake) {
        return m_camera.yaw;
    }
    const Vec3 toParty = middleOf(party) - boss.position;
    if (flatLength(toParty) < 0.5f) {
        return m_camera.yaw;
    }
    const Vec3 side{toParty.x / flatLength(toParty), 0.0f, toParty.z / flatLength(toParty)};
    const Vec3 facing{std::sin(boss.facing), 0.0f, std::cos(boss.facing)};
    const f32 dot = side.x * facing.x + side.z * facing.z;
    // The camera looks the opposite way to the party's side: from them toward the boss.
    // BossCameraStart recomputes this cache. The Dragon's file contains cos(45 degrees),
    // although its authored maxYaw is 18 degrees.
    if (dot < std::cos(record.maxYaw) && record.maxYaw < kPi) {
        const f32 cross = facing.z * side.x - facing.x * side.z;
        const f32 swung = boss.facing + (cross >= 0.0f ? record.maxYaw : -record.maxYaw);
        return wrapAngle(swung + kPi);
    }
    return wrapAngle(std::atan2(side.x, side.z) + kPi);
}

/** How far inside the view's edges the boss (to its radius) and every player stand: the
 * least of the distances to the four side planes; under nought something is cut off. */
f32 BossCamera::viewMargin(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                           const CameraView& view) const {
    const Vec3 forward = m_camera.forward();
    const Vec3 right = m_camera.right();
    const Vec3 up = m_camera.up();
    const f32 tanX = std::tan(view.horizontalFov * 0.5f);
    const f32 tanY = tanX / view.aspect;
    const f32 normX = std::sqrt(1.0f + tanX * tanX);
    const f32 normY = std::sqrt(1.0f + tanY * tanY);
    f32 least = 1000.0f;
    const auto consider = [&](const Vec3& spot, f32 radius) {
        const Vec3 relative = spot - m_camera.position;
        const f32 x = glm::dot(relative, right);
        const f32 y = glm::dot(relative, up);
        const f32 z = glm::dot(relative, forward);
        least = std::min(least, (z * tanX - std::abs(x)) / normX - radius);
        least = std::min(least, (z * tanY - std::abs(y)) / normY - radius);
    };
    consider(boss.position, boss.radius);
    consider(boss.position + Vec3{0.0f, boss.height, 0.0f}, 0.0f);
    for (const CameraSubject& subject : party) {
        consider(subject.feet, 0.0f);
        consider(subject.follow, 0.0f);
    }
    return least;
}

void BossCamera::place() {
    m_camera.roll = 0.0f;
    m_camera.position = m_attention - m_camera.forward() * m_distance;
}

void BossCamera::reset(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                       const BossCameraInfo& record, const CameraView& view) {
    m_stepOwed = 0.0f;
    m_distance = boss.awake ? record.minDistance : record.minPlayerDistance;
    // The first look is along the line from the boss to the party, whether it sleeps or not.
    const Vec3 toParty = middleOf(party) - boss.position;
    m_camera.yaw = flatLength(toParty) > 0.5f ? wrapAngle(std::atan2(toParty.x, toParty.z) + kPi)
                                              : boss.facing;
    m_camera.pitch = record.minPitch;
    m_attention = lookPoint(boss, party, record);
    place();
    // Then it settles: backed off until everyone is in view.
    for (s32 i = 0; i < 200; ++i) {
        const f32 before = m_distance;
        update(boss, party, record, view, 1.0f / kFrameRate);
        if (std::abs(m_distance - before) < 0.01f) {
            break;
        }
    }
}

void BossCamera::update(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                        const BossCameraInfo& record, const CameraView& view, f32 seconds) {
    if (seconds <= 0.0f) {
        return;
    }
    // The look point follows, and the yaw swings at its rate toward the party's line.
    const f32 ease = 1.0f - std::exp(-kEase * seconds);
    m_attention += (lookPoint(boss, party, record) - m_attention) * ease;
    const f32 turn = wrapAngle(wantedYaw(boss, party, record) - m_camera.yaw);
    const f32 most = kTurnRate * seconds;
    m_camera.yaw = wrapAngle(m_camera.yaw + std::clamp(turn, -most, most));
    // Steeper the closer it stands.
    const f32 nearest = boss.awake ? record.minDistance : record.minPlayerDistance;
    const f32 farthest = boss.awake ? record.maxDistance : record.maxPlayerDistance;
    const f32 range = farthest - nearest;
    const f32 t = range > 0.01f ? std::clamp((m_distance - nearest) / range, 0.0f, 1.0f) : 1.0f;
    m_camera.pitch = record.minPitch + (record.maxPitch - record.minPitch) * t;
    place();
    // Frame by frame, as the original steps: out fast when something is cut off, out a
    // little when it is close to the edge, in when there is room to spare.
    m_stepOwed += seconds;
    while (m_stepOwed >= 1.0f / kFrameRate) {
        m_stepOwed -= 1.0f / kFrameRate;
        m_margin = viewMargin(boss, party, view);
        f32 wanted = m_distance;
        if (m_margin < 0.0f) {
            wanted += kOutStep;
        } else if (m_margin < kTightMargin && m_distance < farthest) {
            wanted += 2.0f * (kWantedMargin - m_margin);
        } else if (m_margin > kLooseMargin) {
            wanted -= m_margin - kWantedMargin;
        }
        m_distance = std::clamp(wanted, nearest, farthest * kFarthest);
        place();
    }
}

} // namespace gdl::game
