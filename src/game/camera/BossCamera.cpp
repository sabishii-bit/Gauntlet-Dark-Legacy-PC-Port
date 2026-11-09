#include "game/camera/BossCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

namespace gdl::game {

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr f32 kLookRestart = 0.001f; ///< a look point this near its target is on it
constexpr u32 kBackingOff = 0x200;
constexpr u32 kComingIn = 0x100;

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

f32 BossCamera::limit(f32 value, f32 target, f32 least, f32 most, f32 acceleration, f32 stopScale,
                      f32& velocity, bool wrap, f32 seconds) {
    f32 delta = target - value;
    if (wrap) {
        delta = wrapAngle(delta);
    }
    const f32 distance = std::abs(delta);
    const f32 speed = std::abs(velocity);
    const f32 gain = acceleration * seconds;
    const f32 stop = seconds * stopScale * most;
    if (stopScale > 0.0f && speed < stop && distance < stop) {
        velocity = 0.0f;
    } else if (distance < seconds * (speed + gain)) {
        velocity = delta / seconds; // the last step lands on it
    } else {
        f32 next = 0.0f;
        if (speed > 0.0f && speed / acceleration >= distance / speed) {
            // Brake without reversing when there is less than a stopping distance left.
            next =
                velocity > 0.0f ? std::max(velocity - gain, 0.0f) : std::min(velocity + gain, 0.0f);
        } else {
            next = velocity + (delta > 0.0f ? gain : -gain);
        }
        velocity = std::clamp(next, least, most);
    }
    return value + velocity * seconds;
}

/** Where the camera looks: the boss, offset by the record's near or far offset by how far
 * back the camera stands; the party's middle while the boss sleeps. */
Vec3 BossCamera::lookPoint(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                           const BossCameraInfo& record, const ApproachPolicy* approach) const {
    if (boss.focus != BossCameraSubject::Focus::Combat) {
        return boss.position + boss.attentionOffset +
               (boss.focus == BossCameraSubject::Focus::Wizard ? record.wizardAttention
                                                               : record.keyAttention);
    }
    if (!boss.awake) {
        Vec3 middle = middleOf(party);
        if (approach != nullptr && approach->boundsMidpoint && !party.empty()) {
            Vec3 low = party.front().follow;
            Vec3 high = low;
            for (const auto& player : party) {
                low = glm::min(low, player.follow);
                high = glm::max(high, player.follow);
            }
            middle = (low + high) * 0.5f;
        }
        return approach != nullptr && approach->attentionBounds
                   ? glm::clamp(middle, approach->attentionBounds->first,
                                approach->attentionBounds->second)
                   : middle;
    }
    const f32 range = record.maxDistance - record.minDistance;
    const f32 t =
        range > 0.01f ? std::clamp((m_distance - record.minDistance) / range, 0.0f, 1.0f) : 1.0f;
    // Without either flag the anchor is the base position the creature's geometry gave.
    Vec3 anchor = boss.baseAttention.value_or(boss.position);
    if ((record.flags & BossCameraInfo::kTracksParty) != 0) {
        anchor = middleOf(party);
    } else if ((record.flags & BossCameraInfo::kLiveAnchor) != 0) {
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
    Vec3 side{toParty.x / flatLength(toParty), 0.0f, toParty.z / flatLength(toParty)};
    const Vec3 facing{portable::sin(boss.facing), 0.0f, portable::cos(boss.facing)};
    // BossCameraStart recomputes this cache. The Dragon's file contains cos(45 degrees),
    // although its authored maxYaw is 18 degrees.
    const bool bounded = boss.focus == BossCameraSubject::Focus::Combat && record.maxYaw < kPi;
    const f32 cosMost = bounded ? portable::cos(record.maxYaw) : -2.0f;
    f32 dot = side.x * facing.x + side.z * facing.z;
    if ((record.flags & BossCameraInfo::kFrontOnly) == 0) {
        // The camera may also look from the boss's far side, and keeps
        // to whichever side it is on now: a party crossing the boss's line does not swing
        // it half a turn round.
        const Vec3 fromLook = m_camera.position - m_attention;
        const f32 length = flatLength(fromLook);
        const Vec3 now{length > 0.0f ? fromLook.x / length : 0.0f, 0.0f,
                       length > 0.0f ? fromLook.z / length : 0.0f};
        const auto agreement = [&](const Vec3& way, f32 along) {
            return along >= cosMost ? now.x * way.x + now.z * way.z : -1.0f;
        };
        const Vec3 other = -side;
        const f32 otherDot = -dot;
        if (agreement(other, otherDot) > agreement(side, dot)) {
            side = other;
            dot = otherDot;
        }
    }
    // The camera looks the opposite way to the party's side: from them toward the boss.
    if (bounded && dot < cosMost) {
        const f32 cross = facing.z * side.x - facing.x * side.z;
        const f32 swung = boss.facing + (cross >= 0.0f ? record.maxYaw : -record.maxYaw);
        return wrapAngle(swung + kPi);
    }
    return wrapAngle(portable::atan2(side.x, side.z) + kPi);
}

/** How far inside the view's edges the boss (to its radius) and every player stand: the
 * least of the distances to the four side planes; under nought something is cut off. */
f32 BossCamera::viewMargin(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                           const CameraView& view) const {
    const Vec3 forward = m_camera.forward();
    const Vec3 right = m_camera.right();
    const Vec3 up = m_camera.up();
    const f32 tanX = portable::tan(view.horizontalFov * 0.5f);
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
    // BossCamPlayerCalc fits only the players. Including the sleeping boss pulls the
    // camera back to frame the raised arena before the party has climbed its stairs.
    if (boss.awake || boss.focus != BossCameraSubject::Focus::Combat) {
        consider(boss.position, boss.radius);
        consider(boss.position + Vec3{0.0f, boss.height, 0.0f}, 0.0f);
    }
    for (const CameraSubject& subject : party) {
        consider(subject.feet, 0.0f);
        // Fit the collision sphere, not just its centre: hands and the upper body
        // must remain inside the frame while approaching the camera.
        consider(subject.follow, subject.viewRadius);
    }
    return least;
}

/** How far back the camera wants to be, from how far inside the view's edges everyone
 * stands: out fast when something is cut off, out a little when it is close to the edge,
 * in when there is room to spare, and, once going one way, on until it is well in that
 * band. The distance then moves toward it under its speeds. */
f32 BossCamera::wantedDistance(f32 nearest, f32 farthest, const ApproachPolicy* approach) {
    f32 wanted = m_distance;
    u32 band = 0;
    const f32 loose = approach != nullptr ? approach->inwardLooseMargin : kLooseMargin;
    const f32 held = approach != nullptr ? approach->inwardHeldMargin : kWantedMargin;
    const f32 gain = approach != nullptr ? approach->inwardGain : 1.0f;
    if (m_margin < 0.0f) {
        band = kBackingOff;
        wanted += kOutStep;
        // NOLINTNEXTLINE(bugprone-branch-clone): one branch per retail camera band
    } else if (m_margin < kTightMargin && m_distance < farthest) {
        band = kBackingOff;
        wanted += 2.0f * (kWantedMargin - m_margin);
    } else if (m_margin < kHeldTightMargin && m_distance < farthest &&
               (m_band & kBackingOff) != 0) {
        band = kBackingOff;
        wanted += 2.0f * (kWantedMargin - m_margin);
        // NOLINTNEXTLINE(bugprone-branch-clone): one branch per retail camera band
    } else if (m_margin > loose) {
        band = kComingIn;
        wanted -= gain * (m_margin - kWantedMargin);
    } else if (m_margin > held && (m_band & kComingIn) != 0) {
        band = kComingIn;
        wanted -= gain * (m_margin - held);
    }
    m_band = band;
    return std::clamp(wanted, nearest, farthest * kFarthest);
}

/** The look point goes toward its target from where it set out, its progress under a
 * speed and its direction eased by component; a target gone off to the side restarts
 * it from where it is (BossCamLimitAttn). */
void BossCamera::followLook(const Vec3& target, f32 seconds) {
    Vec3 way = target - m_lookBase;
    const f32 length = glm::length(way);
    if (length <= kLookRestart) {
        m_lookLength = 0.0f;
        m_lookVelocity = 0.0f;
        m_lookDirection = Vec3{0.0f};
        m_lookTurnVelocity = Vec3{0.0f};
        m_attention = target;
        m_lookBase = target;
        return;
    }
    way /= length;
    if (m_lookLength <= 0.0f) {
        m_lookLength = limit(m_lookLength, length, -kLookSpeed, kLookSpeed, kLookAcceleration, 0.0f,
                             m_lookVelocity, false, seconds);
        m_lookDirection = way;
        m_lookTurnVelocity = Vec3{0.0f};
        m_attention = m_lookBase + m_lookDirection * m_lookLength;
        return;
    }
    if (glm::dot(way, m_lookDirection) < kLookTurnReset) {
        m_lookLength = 0.0f;
        m_lookBase = m_attention;
    } else {
        m_lookLength = limit(m_lookLength, length, -kLookSpeed, kLookSpeed, kLookAcceleration, 0.0f,
                             m_lookVelocity, false, seconds);
    }
    for (s32 axis = 0; axis < 3; ++axis) {
        m_lookDirection[axis] =
            limit(m_lookDirection[axis], way[axis], -kLookTurnSpeed, kLookTurnSpeed,
                  kLookTurnAcceleration, 0.0f, m_lookTurnVelocity[axis], false, seconds);
    }
    m_attention = m_lookBase + m_lookDirection * m_lookLength;
}

void BossCamera::place() {
    m_camera.roll = 0.0f;
    m_camera.position = m_attention - m_camera.forward() * m_distance;
}

const WorldLocator* BossCamera::approachMarker(std::span<const WorldLocator> markers,
                                               const Vec3& near) {
    if (markers.empty()) {
        m_marker = -1;
        return nullptr;
    }
    // The boss approach selects by horizontal distance, retaining its marker until
    // another is at least 1.5 times nearer (the BossCamPlayerCalc transmitter query).
    usize best = 0;
    f32 distance = flatLength(near - markers[0].position);
    for (usize i = 1; i < markers.size(); ++i) {
        const f32 candidate = flatLength(near - markers[i].position);
        if (candidate < distance) {
            best = i;
            distance = candidate;
        }
    }
    if (m_marker < 0 || static_cast<usize>(m_marker) >= markers.size() ||
        distance <= 0.667f * flatLength(near - markers[static_cast<usize>(m_marker)].position)) {
        m_marker = static_cast<s32>(best);
    }
    return &markers[static_cast<usize>(m_marker)];
}

void BossCamera::reset(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                       const BossCameraInfo& record, const CameraView& view,
                       std::span<const WorldLocator> markers, const ApproachPolicy* approach) {
    if (boss.awake || boss.focus != BossCameraSubject::Focus::Combat)
        approach = nullptr;
    m_lookLength = 0.0f;
    m_lookVelocity = 0.0f;
    m_lookDirection = Vec3{0.0f};
    m_lookTurnVelocity = Vec3{0.0f};
    m_distanceVelocity = 0.0f;
    m_yawVelocity = 0.0f;
    m_pitchVelocity = 0.0f;
    m_band = 0;
    m_marker = -1;
    m_distance = boss.awake ? record.minDistance : record.minPlayerDistance;
    // With no approach markers, fall back to the line from the boss to the party.
    const Vec3 toParty = middleOf(party) - boss.position;
    m_camera.yaw = flatLength(toParty) > 0.5f
                       ? wrapAngle(portable::atan2(toParty.x, toParty.z) + kPi)
                       : boss.facing;
    m_camera.pitch = record.minPitch;
    if (!boss.awake) {
        const Vec3 near =
            approach != nullptr ? lookPoint(boss, party, record, approach) : middleOf(party);
        if (const auto* marker = approachMarker(markers, near); marker != nullptr) {
            m_camera.yaw = marker->rotation.y;
            m_camera.pitch = marker->rotation.x;
        }
    }
    m_attention = lookPoint(boss, party, record, approach);
    m_lookBase = m_attention;
    place();
    // Then it settles: backed off until everyone is in view.
    for (s32 i = 0; i < 400; ++i) {
        const f32 before = m_distance;
        const f32 pitchBefore = m_camera.pitch;
        update(boss, party, record, view, 1.0f / kFrameRate, markers, approach);
        if (i > 0 && std::abs(m_distance - before) < 0.01f &&
            std::abs(m_camera.pitch - pitchBefore) < 1.0e-6f) {
            break;
        }
    }
}

void BossCamera::update(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                        const BossCameraInfo& record, const CameraView& view, f32 seconds,
                        std::span<const WorldLocator> markers, const ApproachPolicy* approach) {
    if (seconds <= 0.0f) {
        return;
    }
    if (boss.awake || boss.focus != BossCameraSubject::Focus::Combat)
        approach = nullptr;
    const Vec3 target = lookPoint(boss, party, record, approach);
    const WorldLocator* marker =
        !boss.awake ? approachMarker(markers, approach != nullptr ? target : middleOf(party))
                    : nullptr;
    // A distant new look point (the boss waking, the wizard arriving) turns the camera
    // toward its eased attention while retaining at least the current distance. The eye
    // stays put when attention moves away, but backs out when attention comes closer.
    if (glm::distance(target, m_attention) > kLookSpeed * seconds) {
        followLook(target, seconds);
        const Vec3 fromCamera = m_attention - m_camera.position;
        const f32 length = glm::length(fromCamera);
        if (length > 0.0f) {
            m_camera.yaw = wrapAngle(portable::atan2(fromCamera.x, fromCamera.z));
            m_camera.pitch = portable::atan2(-fromCamera.y, flatLength(fromCamera));
            m_distance = std::max(m_distance, length);
        }
        // Both GC BossCameraUpdate (8001c478) and DC 8c082e6a..8c082edc rebuild
        // eye = attention - forward * distance after the transition calculation.
        place();
        m_margin = viewMargin(boss, party, view);
        return;
    }
    followLook(target, seconds);
    // The yaw swings toward the party's line under its speeds.
    const f32 yaw = marker != nullptr ? marker->rotation.y : wantedYaw(boss, party, record);
    m_camera.yaw = wrapAngle(limit(m_camera.yaw, yaw, -kYawSpeed, kYawSpeed, kYawAcceleration,
                                   kStopScale, m_yawVelocity, true, seconds));
    // The distance goes out or comes in by how everyone fits, under its speeds.
    const f32 nearest = boss.awake ? record.minDistance : record.minPlayerDistance;
    // BossCameraUpdate doubles the greatest distance while the good wizard stands, so the
    // party may spread about the arena for the coins without going out of view.
    const f32 approachScale = approach != nullptr ? approach->maxDistanceScale : 1.0f;
    const f32 farthest =
        (boss.awake ? record.maxDistance : record.maxPlayerDistance * approachScale) *
        (boss.focus == BossCameraSubject::Focus::Wizard ? kWizardReach : 1.0f);
    m_margin = viewMargin(boss, party, view);
    const f32 wanted = wantedDistance(nearest, farthest, approach);
    const f32 settled = m_distance;
    m_distance = limit(m_distance, wanted, -kInSpeed, kOutSpeed, kDistanceAcceleration, kStopScale,
                       m_distanceVelocity, false, seconds);
    // Steeper the closer it stands; the pitch waits while the distance is far from where
    // it is going, so the two do not chase each other.
    const f32 range = farthest - nearest;
    const f32 t = range > 0.01f ? std::clamp((settled - nearest) / range, 0.0f, 1.0f) : 1.0f;
    f32 pitch = m_camera.pitch;
    if (marker != nullptr) {
        pitch = marker->rotation.x;
    } else if (std::abs(settled - wanted) < kPitchHold) {
        pitch = record.minPitch + (record.maxPitch - record.minPitch) * t;
    }
    m_camera.pitch =
        wrapAngle(limit(m_camera.pitch, pitch, -kPitchSpeed, kPitchSpeed, kPitchAcceleration,
                        kPitchStopScale, m_pitchVelocity, true, seconds));
    place();
}

} // namespace gdl::game
