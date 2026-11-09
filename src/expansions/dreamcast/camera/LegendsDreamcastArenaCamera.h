#pragma once

#include <span>
#include <vector>

#include "game/camera/BossCamera.h"

namespace gdl::game {

/** Dreamcast's BCAM path. The native dispatcher tries it before the legacy
 * transmitter camera whenever the level contains a boss (8c09e696..e750).
 * The shared boss camera owns the fitting/inertia; this adapter supplies the
 * native subject and approach conventions without altering GDL's policy. */
class LegendsDreamcastArenaCamera {
public:
    void reset(const BossCameraSubject& boss, std::span<const CameraSubject> party,
               const BossCameraInfo& record, const CameraRange& range,
               std::span<const WorldLocator> markers);
    void trackBoss(const BossCameraSubject& boss);
    void freeze();
    void focusOn(Vec3 attention);
    void update(std::span<const CameraSubject> party, f32 seconds);
    const WorldCamera& camera() const { return m_camera; }
    const Vec3& attention() const { return m_attention; }
    WorldCamera sample(f32 extraSeconds = 0) const;
    Vec3 sampleAttention(f32 extraSeconds = 0) const;

private:
    void step(std::span<const CameraSubject> party, bool reset);
    f32 sampleFraction(f32 extraSeconds) const;
    BossCamera m_core;
    BossCameraSubject m_boss;
    BossCameraInfo m_record;
    CameraRange m_range;
    std::vector<WorldLocator> m_markers;
    WorldCamera m_camera, m_previousCamera;
    Vec3 m_attention{0}, m_previousAttention{0};
    f32 m_accumulator = 0;
    bool m_frozen = false;
};

} // namespace gdl::game
