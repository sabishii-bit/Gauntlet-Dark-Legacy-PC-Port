#include "expansions/dreamcast/camera/LegendsDreamcastArenaCamera.h"

#include <algorithm>
#include <cmath>

#include "engine/math/PortableMath.h"

namespace gdl::game {
namespace {
constexpr f32 kStep = 1.0f / 30;
constexpr CameraView kView{kPi / 3, 640.0f / 416};
bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

WorldCamera keepPartyVisible(WorldCamera camera, std::span<const CameraSubject> party) {
    const Vec3 forward = camera.forward();
    const Vec3 right = camera.right();
    const Vec3 up = camera.up();
    const f32 tanX = portable::tan(kView.horizontalFov * .5f);
    const f32 tanY = tanX / kView.aspect;
    const f32 normX = std::sqrt(1 + tanX * tanX);
    const f32 normY = std::sqrt(1 + tanY * tanY);
    f32 retreat = 0;
    const auto fit = [&](Vec3 point, f32 radius) {
        const Vec3 relative = point - camera.position;
        const f32 z = glm::dot(relative, forward);
        retreat = std::max(retreat, WorldCamera::kNear + radius - z);
        retreat =
            std::max(retreat, (std::abs(glm::dot(relative, right)) + radius * normX) / tanX - z);
        retreat = std::max(retreat, (std::abs(glm::dot(relative, up)) + radius * normY) / tanY - z);
    };
    for (const auto& subject : party) {
        fit(subject.feet, 0);
        fit(subject.follow, std::max(subject.viewRadius, 0.0f));
    }
    // Port safety policy, not a reconstructed native instruction: BCAM's
    // inertial focus change can briefly outrun its distance acceleration.
    // Retreat only the presented eye by the analytic amount needed to keep
    // the party inside the canonical frustum. The core state, angles, focus,
    // movement limits and authored constants remain untouched.
    if (retreat > 0) {
        camera.position -= forward * (retreat + .001f);
    }
    return camera;
}
} // namespace

void LegendsDreamcastArenaCamera::reset(const BossCameraSubject& boss,
                                        std::span<const CameraSubject> party,
                                        const BossCameraInfo& record, const CameraRange& range,
                                        std::span<const WorldLocator> markers) {
    *this = {};
    m_boss = boss;
    m_record = record;
    m_range = range;
    for (const auto& marker : markers) {
        if (marker.kind == LocatorKind::CameraGame && finite(marker.position) &&
            finite(marker.rotation)) {
            m_markers.push_back(marker);
        }
    }
    step(party, true);
    m_previousCamera = m_camera;
    m_previousAttention = m_attention;
}

void LegendsDreamcastArenaCamera::trackBoss(const BossCameraSubject& boss) {
    if (!finite(boss.position) || !finite(boss.attentionOffset) || !std::isfinite(boss.facing) ||
        !std::isfinite(boss.radius) || boss.radius < 0) {
        return;
    }
    m_boss = boss;
}

void LegendsDreamcastArenaCamera::step(std::span<const CameraSubject> party, bool reset) {
    if (party.empty()) {
        return;
    }
    BossCameraInfo record = m_record;
    BossCameraSubject boss = m_boss;
    // Native 8c081540 fits the TYPE-radius sphere at actor+0x40. Supplying
    // that center with zero extra height avoids adding a second floor-root
    // sphere, while the camera's initialized anchor remains independent.
    boss.position += boss.attentionOffset;
    boss.attentionOffset = Vec3{0};
    boss.height = 0;
    if ((record.flags & BossCameraInfo::kTracksParty) != 0 && boss.awake) {
        // GetBossAvgPos8c081320, r7=1 at8c082420: the midpoint of player
        // and boss attention bounds, not an arithmetic average of the party.
        Vec3 low = boss.position;
        Vec3 high = low;
        for (const auto& player : party) {
            low = glm::min(low, player.follow);
            high = glm::max(high, player.follow);
        }
        record.flags &= ~(BossCameraInfo::kTracksParty | BossCameraInfo::kLiveAnchor);
        boss.baseAttention = (low + high) * .5f;
    }
    BossCamera::ApproachPolicy approach;
    approach.maxDistanceScale = 1.5f;
    approach.inwardLooseMargin = 2.5f;
    approach.inwardHeldMargin = 2.25f;
    approach.inwardGain = 2;
    approach.boundsMidpoint = true;
    approach.attentionBounds = std::pair{m_range.boundsMin, m_range.boundsMax};
    m_previousCamera = m_camera;
    m_previousAttention = m_attention;
    if (reset) {
        m_core.reset(boss, party, record, kView, m_markers, &approach);
    } else {
        m_core.update(boss, party, record, kView, kStep, m_markers, &approach);
    }
    m_camera = keepPartyVisible(m_core.camera(), party);
    m_attention = m_core.attention();
}

void LegendsDreamcastArenaCamera::update(std::span<const CameraSubject> party, f32 seconds) {
    if (m_frozen || !std::isfinite(seconds) || seconds <= 0) {
        return;
    }
    m_accumulator += std::min(seconds, .5f);
    while (m_accumulator + .000001f >= kStep) {
        m_accumulator = std::max(m_accumulator - kStep, 0.0f);
        step(party, false);
    }
}

void LegendsDreamcastArenaCamera::focusOn(Vec3 attention) {
    if (!finite(attention)) {
        return;
    }
    m_frozen = true;
    m_attention = attention;
    const Vec3 direction = attention - m_camera.position;
    if (glm::length(direction) > .001f) {
        m_camera.yaw = std::atan2(direction.x, direction.z);
        m_camera.pitch = std::atan2(-direction.y, std::hypot(direction.x, direction.z));
    }
    m_previousCamera = m_camera;
    m_previousAttention = m_attention;
    m_accumulator = 0;
}

void LegendsDreamcastArenaCamera::freeze() {
    m_frozen = true;
    m_previousCamera = m_camera;
    m_previousAttention = m_attention;
    m_accumulator = 0;
}

f32 LegendsDreamcastArenaCamera::sampleFraction(f32 extraSeconds) const {
    return std::clamp((m_accumulator + (std::isfinite(extraSeconds) ? extraSeconds : 0)) / kStep,
                      0.0f, 1.0f);
}

WorldCamera LegendsDreamcastArenaCamera::sample(f32 extraSeconds) const {
    return m_camera.interpolate(m_previousCamera, sampleFraction(extraSeconds));
}

Vec3 LegendsDreamcastArenaCamera::sampleAttention(f32 extraSeconds) const {
    return glm::mix(m_previousAttention, m_attention, sampleFraction(extraSeconds));
}
} // namespace gdl::game
