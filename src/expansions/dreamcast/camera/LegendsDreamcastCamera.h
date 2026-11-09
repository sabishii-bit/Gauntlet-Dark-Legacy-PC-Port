#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/world/WorldCamera.h"

#include "game/camera/TowerCamera.h"

namespace gdl {
namespace game {

/** Dreamcast's playable tower camera, separate from its ordinary level
 * transmitter camera. The first transmitter is the hub of a radial view, not
 * the angle of an over-the-shoulder follow camera. Native movement admission
 * uses the completed eye, never an interpolated/local presentation camera. */
class LegendsDreamcastTowerCamera {
public:
    static constexpr f32 kHorizontalFov = 80.0f * kPi / 180.0f;

    void reset(std::span<const CameraSubject> subjects, Vec3 hub);
    void update(std::span<const CameraSubject> subjects, f32 seconds);
    const WorldCamera& camera() const { return m_camera; }
    const Vec3& attention() const { return m_attention; }
    /** Read-only presentation between completed native 30 Hz poses. Extra time
     * is the render fraction of the caller's fixed tick, not a simulation step. */
    WorldCamera sample(f32 extraSeconds = 0) const;
    Vec3 sampleAttention(f32 extraSeconds = 0) const;
    /** Remove only motion towards the native eye inside its 12.5-unit radius.
     * Tangential/outward movement and vertical motion remain unrestricted. */
    Vec3 constrainMovement(Vec3 from, Vec3 to) const;

private:
    void step(std::span<const CameraSubject> subjects, bool snap);
    f32 sampleFraction(f32 extraSeconds) const;
    Vec3 m_hub{0, 3, 0};
    Vec3 m_attention{0, 3, 1};
    Vec3 m_previousAttention{0, 3, 1};
    Vec3 m_relative{0, 0, 1};
    WorldCamera m_camera;
    WorldCamera m_previousCamera;
    f32 m_sumnerBlend = 0;
    f32 m_templeBlend = 0;
    f32 m_accumulator = 0;
    bool m_initialized = false;
};

/** Ordinary Dreamcast levels use authored transmitters and a separate entrance
 * camera. Neither character growth nor decorative mesh collision moves this
 * eye: the native transmitter branch bypasses the free-camera wall solver. */
class LegendsDreamcastLevelCamera {
public:
    void reset(std::span<const CameraSubject> subjects, std::span<const WorldLocator> markers,
               const CameraRange& range, bool opening = true);
    void update(std::span<const CameraSubject> subjects, f32 seconds);
    /** Native post-boss subject switch keeps the completed eye, clears follow
     * velocity/history, and holds its attention until the scene departs. */
    void focusOn(Vec3 attention);
    const WorldCamera& camera() const { return m_camera; }
    const Vec3& attention() const { return m_attention; }
    WorldCamera sample(f32 extraSeconds = 0) const;
    Vec3 sampleAttention(f32 extraSeconds = 0) const;
    bool introActive() const { return m_introActive; }
    s32 marker() const { return m_marker; }

private:
    bool partyMiddle(std::span<const CameraSubject> subjects, Vec3& point, usize& count) const;
    Vec3 bounded(Vec3 point) const;
    void chooseMarker(Vec3 point);
    void step(std::span<const CameraSubject> subjects);
    void aim();
    f32 sampleFraction(f32 extraSeconds) const;

    std::vector<WorldLocator> m_markers;
    CameraRange m_range;
    WorldCamera m_camera;
    WorldCamera m_previousCamera;
    WorldCamera m_followCamera;
    Vec3 m_attention{0};
    Vec3 m_previousAttention{0};
    Vec3 m_followAttention{0};
    std::array<Vec3, 6> m_history{};
    usize m_historyCount = 0;
    usize m_historyIndex = 0;
    s32 m_startMarker = -1;
    s32 m_marker = -1;
    f32 m_distance = 20;
    f32 m_accumulator = 0;
    f32 m_followSpeed = 0;
    s32 m_introHold = 0;
    bool m_introActive = false;
    std::optional<Vec3> m_ceremonyAttention;
};

} // namespace game
} // namespace gdl
