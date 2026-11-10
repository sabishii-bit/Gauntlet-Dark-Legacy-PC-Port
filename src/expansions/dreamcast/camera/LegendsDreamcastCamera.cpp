#include "expansions/dreamcast/camera/LegendsDreamcastCamera.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gdl::game {
namespace {
constexpr f32 kStep = 1.0f / 30.0f;

bool validPoint(Vec3 point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z) &&
           glm::all(glm::lessThanEqual(glm::abs(point), Vec3{100000}));
}

f32 blendPhase(f32 previous, bool active, bool snap) {
    if (snap) {
        return active ? kPi : 0.0f;
    }
    return std::clamp(previous + (active ? 4.0f : -4.0f) * kStep, 0.0f, kPi);
}
} // namespace

void LegendsDreamcastTowerCamera::reset(std::span<const CameraSubject> subjects, Vec3 hub) {
    m_hub = validPoint(hub) ? hub : Vec3{0, 3, 0};
    m_hub.y = std::max(m_hub.y, 3.0f);
    m_accumulator = 0;
    m_sumnerBlend = 0;
    m_templeBlend = 0;
    m_relative = {0, 0, 1};
    m_attention = m_hub + m_relative;
    m_camera = {};
    m_camera.position = m_hub;
    m_initialized = false;
    step(subjects, true);
}

void LegendsDreamcastTowerCamera::update(std::span<const CameraSubject> subjects, f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    // Camera filtering runs at the native simulation rate. Rendering samples
    // the previous/current completed cameras without advancing this state.
    m_accumulator += std::min(seconds, .5f);
    while (m_accumulator + 1.0e-6f >= kStep) {
        m_accumulator = std::max(m_accumulator - kStep, 0.0f);
        step(subjects, !m_initialized);
    }
}

void LegendsDreamcastTowerCamera::step(std::span<const CameraSubject> subjects, bool snap) {
    // Only completed native updates advance this pair. A caller ticking at
    // 60 Hz must not discard the previous 30 Hz pose on each intervening tick.
    m_previousCamera = m_camera;
    m_previousAttention = m_attention;
    Vec3 weighted{0};
    f32 weights = 0;
    f32 radius = 0;
    bool nearSumner = false;
    usize count = 0;
    // 1ST_READ.BIN 8c07f700 dispatches stage 0x0d00 to 8c07e580,
    // rather than the standard 8c07db00 transmitter-follow handler.
    // 8c07e7b6..8c07eab4 uses at most four targets relative to transmitter
    // zero, with a 42..52 band weighting players entering the outer rooms.
    for (const auto& subject : subjects) {
        if (!validPoint(subject.feet) || count == 4) {
            continue;
        }
        // The adapter supplies a fixed character attention height. Do not
        // substitute follow/viewRadius: those grow with Dark Legacy levels,
        // unlike the Legends tower's native character framing.
        const Vec3 relative = subject.feet + Vec3{0, 3, 0} - m_hub;
        const f32 distance = glm::length(relative);
        const f32 weight =
            distance > 0.001f
                ? (distance > 52.0f ? 10.0f : std::max(distance - 42.0f, 1.0f)) / distance
                : 1.0f;
        weighted += relative * weight;
        weights += weight;
        // 8c07e7fe..808 retains the nearest target index; 8c07e8ac..8b0
        // selects that radius for the eye/ridge, not the farthest member.
        radius = count == 0 ? distance : std::min(radius, distance);
        const Vec2 sumnerDelta{subject.feet.x, subject.feet.z - 9.75f};
        nearSumner |= glm::dot(sumnerDelta, sumnerDelta) < 5.0f;
        ++count;
    }
    if (count == 0 || weights <= 0) {
        return;
    }
    const Vec3 relative = weighted / weights;
    const f32 ridge = std::max(std::min(radius, 64.0f - radius) - 12.0f, 0.0f);
    Vec3 target = m_hub + relative;
    // 8c07ece0 raises the attention in the middle radial band. The native
    // tower has separate Sumner and temple approaches (8c07eb4a..8c07ece0).
    target.y += .45f * ridge - 1.0f;
    const Vec2 templeDelta{target.x + 14.0f, target.z + 44.0f};
    m_sumnerBlend = blendPhase(m_sumnerBlend, nearSumner, snap);
    m_templeBlend = blendPhase(m_templeBlend, glm::dot(templeDelta, templeDelta) < 70.0f, snap);
    const f32 sumnerLift = .5f - .5f * std::cos(m_sumnerBlend);
    const f32 templeApproach = .5f - .5f * std::cos(m_templeBlend);
    Vec3 eye = m_hub;
    eye.y += std::max((radius - 12.0f) * .7f, 0.0f) + sumnerLift;
    eye.z -= 15.0f * templeApproach;
    Vec3 direction = target - eye;
    const f32 length = glm::length(direction);
    if (length > .001f) {
        direction /= length;
    } else {
        direction = {m_relative.x, 0, m_relative.z};
        direction = glm::length(direction) > .001f ? glm::normalize(direction) : Vec3{0, 0, 1};
    }
    // 8c07ee36..8c07eef4 keeps the eye inside the tower and advances it
    // towards an outer room as the party leaves the hub. The 20..26 CAMS
    // radius and nearest transmitter yaw are not used by this mode.
    const f32 approach =
        std::max(radius - (12.0f + 1.25f * ridge) + .3f * std::max(radius - 64.0f, 0.0f), 0.0f);
    eye += direction * std::min(approach, std::max(length - 4.0f, 0.0f));
    m_relative = relative;
    m_attention = target;
    m_camera.position = snap ? eye : glm::mix(m_camera.position, eye, 1.0f / 3.0f);
    direction = m_attention - m_camera.position;
    const f32 horizontal = std::hypot(direction.x, direction.z);
    if (glm::length(direction) > .001f) {
        m_camera.pitch = std::atan2(-direction.y, horizontal);
        if (horizontal > .001f) {
            m_camera.yaw = std::atan2(direction.x, direction.z);
        }
    }
    m_initialized = true;
    if (snap) {
        m_previousCamera = m_camera;
        m_previousAttention = m_attention;
    }
}

f32 LegendsDreamcastTowerCamera::sampleFraction(f32 extraSeconds) const {
    const f32 extra = std::isfinite(extraSeconds) ? std::max(extraSeconds, 0.0f) : 0;
    return std::clamp((m_accumulator + extra) / kStep, 0.0f, 1.0f);
}

WorldCamera LegendsDreamcastTowerCamera::sample(f32 extraSeconds) const {
    return m_camera.interpolate(m_previousCamera, sampleFraction(extraSeconds));
}

Vec3 LegendsDreamcastTowerCamera::sampleAttention(f32 extraSeconds) const {
    return glm::mix(m_previousAttention, m_attention, sampleFraction(extraSeconds));
}

Vec3 LegendsDreamcastTowerCamera::constrainMovement(Vec3 from, Vec3 to) const {
    if (!m_initialized || !validPoint(from) || !validPoint(to)) {
        return to;
    }
    // Dreamcast CameraLimitPlayerDpos (8c07c560), tower stage 0x0d00:
    // 8c07c6a0..72e projects out inward horizontal motion within 12.5 units
    // of the camera. This applies even to one player; it is not a world wall.
    // 8c0744c0..d4 copies the completed eye into the matrix translation read
    // here. Render interpolation and near-plane collision must not affect it.
    const Vec3 radial{from.x - m_camera.position.x, 0, from.z - m_camera.position.z};
    const f32 distance = glm::length(radial);
    if (distance >= 12.5f || distance <= .001f) {
        // An imported save/console placement exactly at the eye must still
        // be able to leave it; there is no defined inward direction there.
        return to;
    }
    const Vec3 direction = radial / distance;
    const f32 inward = glm::dot(to - from, direction);
    return inward < 0 ? to - direction * inward : to;
}

bool LegendsDreamcastLevelCamera::partyMiddle(std::span<const CameraSubject> subjects, Vec3& point,
                                              usize& count) {
    Vec3 low{0};
    Vec3 high{0};
    count = 0;
    for (const auto& subject : subjects) {
        if (!validPoint(subject.feet) || count == 4) {
            continue;
        }
        low = count == 0 ? subject.feet : glm::min(low, subject.feet);
        high = count == 0 ? subject.feet : glm::max(high, subject.feet);
        ++count;
    }
    // Keep the adapter's existing player-feet anchor until native pdata
    // attention offsets are supplied; the level-scaled GDL follow point is
    // not a substitute for Dreamcast's registered object attention field.
    point = (low + high) * .5f;
    return count != 0;
}

Vec3 LegendsDreamcastLevelCamera::bounded(Vec3 point) const {
    for (usize axis = 0; axis < 3; ++axis) {
        const auto component = static_cast<glm::length_t>(axis);
        if (m_range.boundsMax[component] > m_range.boundsMin[component]) {
            point[component] = std::clamp(point[component], m_range.boundsMin[component],
                                          m_range.boundsMax[component]);
        }
    }
    return point;
}

void LegendsDreamcastLevelCamera::chooseMarker(Vec3 point) {
    s32 nearest = -1;
    s32 second = m_marker;
    f32 best = std::numeric_limits<f32>::max();
    f32 next = best;
    // 8c07b180 compares the current transmitter with its nearest alternative.
    // Height is penalized twice, so a bridge's lower path cannot steal its eye.
    for (usize i = 0; i < m_markers.size(); ++i) {
        if (static_cast<s32>(i) == m_startMarker) {
            continue;
        }
        const Vec3 delta = point - m_markers[i].position;
        const f32 distance = glm::length(delta) + std::abs(delta.y);
        if (static_cast<s32>(i) == m_marker) {
            next = distance;
        } else if (distance < best) {
            if (m_marker < 0) {
                second = nearest;
                next = best;
            }
            nearest = static_cast<s32>(i);
            best = distance;
        } else if (m_marker < 0 && distance < next) {
            second = static_cast<s32>(i);
            next = distance;
        }
    }
    if (nearest < 0) {
        return;
    }
    if (second < 0) {
        m_marker = nearest;
        return;
    }
    if (next < best) {
        std::swap(nearest, second);
    }
    const Vec3 a = m_markers[static_cast<usize>(nearest)].position;
    const Vec3 line = m_markers[static_cast<usize>(second)].position - a;
    const f32 lengthSquared = glm::dot(line, line);
    const f32 ratio = lengthSquared > .000001f
                          ? std::clamp(glm::dot(point - a, line) / lengthSquared, 0.0f, 1.0f)
                          : 0;
    const f32 threshold = m_marker < 0 ? .5f : 1.0f / 3.0f;
    if (ratio <= threshold) {
        m_marker = nearest;
    } else if (ratio >= 1.0f - threshold) {
        m_marker = second;
    }
}

void LegendsDreamcastLevelCamera::reset(std::span<const CameraSubject> subjects,
                                        std::span<const WorldLocator> markers,
                                        const CameraRange& range, bool opening) {
    *this = {};
    m_range = range;
    if (!std::isfinite(m_range.radiusMin) || m_range.radiusMin <= 0) {
        m_range.radiusMin = 20;
    }
    if (!std::isfinite(m_range.radiusMax) || m_range.radiusMax < m_range.radiusMin) {
        m_range.radiusMax = m_range.radiusMin;
    }
    if (!std::isfinite(m_range.minPitch)) {
        m_range.minPitch = .7f;
    }
    if (!validPoint(m_range.boundsMin) || !validPoint(m_range.boundsMax)) {
        m_range.boundsMin = Vec3{0};
        m_range.boundsMax = Vec3{0};
    }
    for (const auto& marker : markers) {
        if ((marker.kind != LocatorKind::CameraGame && marker.kind != LocatorKind::CameraStart) ||
            !validPoint(marker.position) || !validPoint(marker.rotation)) {
            continue;
        }
        if (marker.kind == LocatorKind::CameraStart && m_startMarker < 0) {
            m_startMarker = static_cast<s32>(m_markers.size());
        }
        m_markers.push_back(marker);
    }
    Vec3 middle{0};
    usize count = 0;
    partyMiddle(subjects, middle, count);
    m_followAttention = bounded(middle);
    chooseMarker(middle);
    m_followCamera.pitch = m_range.minPitch;
    if (m_marker >= 0) {
        m_followCamera.yaw = m_markers[static_cast<usize>(m_marker)].rotation.y;
        m_followCamera.pitch = m_markers[static_cast<usize>(m_marker)].rotation.x;
    }
    if (count > 1) {
        m_followCamera.pitch = std::max(m_followCamera.pitch, m_range.minPitch);
    }
    // The native single-player path returns radiusMin (8c076eee). Until the
    // projected multi-player roster is adapted, retain the authored maximum
    // as a conservative party fit rather than coupling it to local video FOV.
    m_distance = count > 1 ? m_range.radiusMax : m_range.radiusMin;
    m_followCamera.position = m_followAttention - m_followCamera.forward() * m_distance;
    m_attention = m_followAttention;
    m_camera = m_followCamera;
    if (opening && count != 0 && m_startMarker >= 0) {
        const auto& start = m_markers[static_cast<usize>(m_startMarker)];
        // InitCameras 8c07bf84 disables the first entry transmitter for normal
        // following. 8c07c0a4..25a builds a separate camera at that exact eye,
        // with an attention along its authored direction, one start-distance
        // away. The two yaw half-turns cancel, as in the ordinary orbit path.
        m_camera = {start.position, start.rotation.x, start.rotation.y, start.rotation.z};
        m_attention = start.position + m_camera.forward() * glm::distance(start.position, middle);
        m_introHold = 91; // 8c07c2ea; native clock units are sixtieths.
        m_introActive = true;
    }
    m_previousCamera = m_camera;
    m_previousAttention = m_attention;
}

void LegendsDreamcastLevelCamera::update(std::span<const CameraSubject> subjects, f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    m_accumulator += std::min(seconds, .5f);
    while (m_accumulator + .000001f >= kStep) {
        m_accumulator = std::max(m_accumulator - kStep, 0.0f);
        step(subjects);
    }
}

void LegendsDreamcastLevelCamera::focusOn(Vec3 attention) {
    if (!validPoint(attention) || m_ceremonyAttention == attention) {
        return;
    }
    // CameraAddSubject 8c076020 -> 8c075ec0 copies the new attention and
    // clears velocity, retaining the eye at camera+4c. Skorne does not call
    // this path: its runestone presentation retains the arena camera.
    m_ceremonyAttention = attention;
    m_attention = attention;
    m_followAttention = attention;
    m_history = {};
    m_historyCount = 0;
    m_historyIndex = 0;
    m_followSpeed = 0;
    m_introActive = false;
    m_accumulator = 0;
    aim();
    m_previousCamera = m_camera;
    m_previousAttention = m_attention;
}

void LegendsDreamcastLevelCamera::aim() {
    const Vec3 direction = m_attention - m_camera.position;
    const f32 horizontal = std::hypot(direction.x, direction.z);
    if (glm::length(direction) > .001f) {
        m_camera.pitch = std::atan2(-direction.y, horizontal);
        if (horizontal > .001f) {
            m_camera.yaw = std::atan2(direction.x, direction.z);
        }
    }
}

void LegendsDreamcastLevelCamera::step(std::span<const CameraSubject> subjects) {
    if (m_ceremonyAttention) {
        return;
    }
    Vec3 middle{0};
    usize count = 0;
    if (!partyMiddle(subjects, middle, count)) {
        return;
    }
    m_previousCamera = m_camera;
    m_previousAttention = m_attention;
    chooseMarker(middle);
    const auto advanceAngle = [](f32 current, f32 target, f32 fraction, f32 minimum) {
        const f32 delta = std::remainder(target - current, kTwoPi);
        const f32 step = std::min(std::abs(delta), std::max(std::abs(delta) * fraction, minimum));
        return std::remainder(current + std::copysign(step, delta), kTwoPi);
    };
    if (m_marker >= 0) {
        const Vec3 angles = m_markers[static_cast<usize>(m_marker)].rotation;
        // 8c07b7a0 and 8c077b80, two 60 Hz ticks per completed 30 Hz update.
        m_followCamera.yaw = advanceAngle(m_followCamera.yaw, angles.y, .025f, .0087266472f);
        const f32 pitch = count > 1 ? std::max(angles.x, m_range.minPitch) : angles.x;
        m_followCamera.pitch = advanceAngle(m_followCamera.pitch, pitch, .1f, .0041887904f);
    }
    const f32 desiredDistance = count > 1 ? m_range.radiusMax : m_range.radiusMin;
    const f32 distanceDelta = desiredDistance - m_distance;
    const f32 distanceStep =
        std::min(2.0f, .2f + std::max(std::abs(distanceDelta) - .2f, 0.0f) * .2f);
    m_distance += std::copysign(std::min(std::abs(distanceDelta), distanceStep), distanceDelta);
    // Native 8c077e80 averages six targets, not GDL's nine-frame ring.
    // 8c075c46 bypasses CAMS attention limits during the entrance pan.
    m_history[m_historyIndex] = m_introActive ? middle : bounded(middle);
    m_historyIndex = (m_historyIndex + 1) % m_history.size();
    m_historyCount = std::min(m_historyCount + 1, m_history.size());
    Vec3 target{0};
    for (usize i = 0; i < m_historyCount; ++i) {
        target += m_history[i];
    }
    target /= static_cast<f32>(m_historyCount);
    // 8c076eea sets single-player screen occupancy to .55, below the .6
    // threshold in 8c07d200: 1.4 units/step, with a 16-unit arrival region.
    // This is not player speed. Boss arenas use the separate BCAM path.
    constexpr f32 kSpeed = 1.4f;
    constexpr f32 kArrival = 16.0f;
    m_followSpeed += std::clamp(kSpeed - m_followSpeed, -.6f, .6f);
    const Vec3 delta = target - m_followAttention;
    const f32 distance = glm::length(delta);
    m_followAttention +=
        distance <= m_followSpeed ? delta : delta * (m_followSpeed / std::min(distance, kArrival));
    m_followCamera.position = m_followAttention - m_followCamera.forward() * m_distance;
    if (!m_introActive) {
        m_camera = m_followCamera;
        m_attention = m_followAttention;
        return;
    }
    if (m_introHold > 1) {
        m_introHold = std::max(m_introHold - 2, 1);
        if (m_introHold > 1) {
            return;
        }
    }
    // 8c078de0 moves eye/look-at independently once the 91-tick title hold
    // expires. Both must arrive before handing the view to the follow camera.
    const auto converge = [](Vec3& current, Vec3 goal) {
        const Vec3 delta = goal - current;
        const f32 distance = glm::length(delta);
        if (distance < .3f) {
            current = goal;
            return true;
        }
        current += delta * std::min(1.0f, 2.0f / std::min(distance, 20.0f));
        return false;
    };
    const bool eyeDone = converge(m_camera.position, m_followCamera.position);
    const bool attentionDone = converge(m_attention, m_followAttention);
    aim();
    if (eyeDone && attentionDone) {
        m_introActive = false;
        m_camera = m_followCamera;
        m_attention = m_followAttention;
    }
}

f32 LegendsDreamcastLevelCamera::sampleFraction(f32 extraSeconds) const {
    const f32 extra = std::isfinite(extraSeconds) ? std::max(extraSeconds, 0.0f) : 0;
    return std::clamp((m_accumulator + extra) / kStep, 0.0f, 1.0f);
}

WorldCamera LegendsDreamcastLevelCamera::sample(f32 extraSeconds) const {
    return m_camera.interpolate(m_previousCamera, sampleFraction(extraSeconds));
}

Vec3 LegendsDreamcastLevelCamera::sampleAttention(f32 extraSeconds) const {
    return glm::mix(m_previousAttention, m_attention, sampleFraction(extraSeconds));
}

} // namespace gdl::game
