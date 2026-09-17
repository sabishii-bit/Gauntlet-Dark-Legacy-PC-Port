#pragma once

#include <array>
#include <span>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {

/** A character the camera keeps in view: where it stands and the point it follows. */
struct CameraSubject {
    Vec3 feet{0.0f, 0.0f, 0.0f};
    Vec3 follow{0.0f, 0.0f, 0.0f}; ///< the body's centre, part way up
};

/** The level's camera data: how far the camera stays and how flat it may look. */
struct CameraRange {
    f32 radiusMin = 24.0f;
    f32 radiusMax = 32.0f;
    f32 minPitch = 0.6981317f;        ///< a party is looked down on by at least this angle
    Vec3 boundsMin{0.0f, 0.0f, 0.0f}; ///< the look-at point stays inside this box when it
    Vec3 boundsMax{0.0f, 0.0f, 0.0f}; ///< has any size
};

/** The projection the camera must fit the party into. */
struct CameraView {
    f32 horizontalFov = 1.0471976f; ///< radians
    f32 aspect = 640.0f / 448.0f;
};

/**
 * The tower's follow camera, as the original's standard camera works: it looks at the middle
 * of the party from the angle of the nearest camera marker, turning to a new marker's angle
 * over a short while, keeps the look-at point and its distance smoothed over the last few
 * frames, and backs away just far enough to keep every player on screen.
 */
class TowerCamera {
public:
    static constexpr s32 kRing = 9;         ///< frames the look-at point and distance average over
    static constexpr s32 kTurnSteps = 50;   ///< steps a turn to a new marker takes
    static constexpr f32 kStepRate = 30.0f; ///< steps per second
    static constexpr f32 kSwitchRatio = 4.0f / 9.0f; ///< a marker takes over when this much nearer (squared)
    static constexpr f32 kAloneDistance = 0.8f;    ///< of the shortest range with one player
    static constexpr f32 kPartyDistance = 0.9375f; ///< of the longest range with more
    static constexpr f32 kNearGap = 10.0f; ///< slack kept when the fit lets the camera come in
    static constexpr f32 kFarGap = 4.0f;   ///< slack added when it must go out
    static constexpr s32 kSettleLimit = 100; ///< updates a fresh start may take to settle

    /** Stands the camera up for `subjects` and lets it settle, like a level start. */
    void reset(std::span<const CameraSubject> subjects, std::span<const WorldLocator> markers,
               const CameraRange& range, const CameraView& view);

    /** Follows `subjects` for `seconds`; true while anything is still moving. */
    bool update(std::span<const CameraSubject> subjects, std::span<const WorldLocator> markers,
                const CameraRange& range, const CameraView& view, f32 seconds);

    const WorldCamera& camera() const { return m_camera; }
    f32 yaw() const { return m_camera.yaw; }
    f32 pitch() const { return m_camera.pitch; }
    f32 distance() const { return m_distance; }
    const Vec3& attention() const { return m_attention; }
    s32 marker() const { return m_marker; }
    bool turning() const { return m_turnStep < static_cast<f32>(kTurnSteps); }

    /** The angles a marker asks for: the camera looks the way the marker faces. */
    static f32 markerYaw(const WorldLocator& marker) { return marker.rotation.y; }
    static f32 markerPitch(const WorldLocator& marker) { return marker.rotation.x; }

private:
    static Vec3 middleOf(std::span<const CameraSubject> subjects, const CameraRange& range);
    void chooseMarker(std::span<const WorldLocator> markers, const Vec3& near);
    void aim(std::span<const WorldLocator> markers, usize count, const CameraRange& range,
             bool jump);
    bool turn(f32 steps);
    bool follow();
    f32 fitDistance(const Vec3& point, std::span<const CameraSubject> subjects,
                    const CameraView& view, const CameraRange& range, f32 current) const;
    void place();

    WorldCamera m_camera;
    Vec3 m_attention{0.0f, 0.0f, 0.0f};
    f32 m_distance = 0.0f;
    s32 m_marker = -1;
    s32 m_aimedMarker = -1;
    f32 m_yawRate = 0.0f;
    f32 m_pitchRate = 0.0f;
    f32 m_turnStep = static_cast<f32>(kTurnSteps);
    std::array<Vec3, kRing> m_ringPositions{};
    std::array<f32, kRing> m_ringDistances{};
    s32 m_ringIndex = 0;
};

} // namespace gdl::game
