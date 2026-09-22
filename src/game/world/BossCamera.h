#pragma once

#include <span>

#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

#include "game/world/TowerCamera.h"

namespace gdl::game {

/** The boss as the camera sees it: where it stands, which way it faces, how big it is, and
 * whether the fight is on. */
struct BossCameraSubject {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 facing = 0.0f;
    f32 radius = 4.0f;
    f32 height = 4.0f; ///< of its body's centre, which is kept in view
    bool awake = false;
};

/**
 * The camera of a boss fight, by the level's boss camera record: it looks at the boss (a
 * little off it, nearer its feet the further back it stands) from behind the party, along
 * the party's line to the boss, swung no further about the boss's facing than the record
 * allows; it looks down at the record's pitch, steeper the closer it is; and it backs off
 * or comes in, within the record's distances, so that the boss and every player stay in
 * view. Before the boss wakes it keeps the same angle on the party alone, at the record's
 * player distances.
 */
class BossCamera {
public:
    static constexpr f32 kFrameRate = 30.0f;    ///< the original's steps are per frame
    static constexpr f32 kOutStep = 10.0f;      ///< units back a frame when something is off screen
    static constexpr f32 kTightMargin = 2.0f;   ///< within this of the edge it backs off
    static constexpr f32 kWantedMargin = 2.5f;  ///< the slack it settles at
    static constexpr f32 kLooseMargin = 4.0f;   ///< past this it comes in
    static constexpr f32 kFarthest = 2.0f;      ///< of the record's greatest distance
    static constexpr f32 kTurnRate = 2.0f;      ///< radians a second the yaw may swing
    static constexpr f32 kEase = 6.0f;          ///< how quickly the look point follows

    /** Stands the camera up at once, as the level starts. */
    void reset(const BossCameraSubject& boss, std::span<const CameraSubject> party,
               const BossCameraInfo& record, const CameraView& view);
    void update(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                const BossCameraInfo& record, const CameraView& view, f32 seconds);

    const WorldCamera& camera() const { return m_camera; }
    f32 yaw() const { return m_camera.yaw; }
    f32 pitch() const { return m_camera.pitch; }
    f32 distance() const { return m_distance; }
    const Vec3& attention() const { return m_attention; }
    /** How far, in units, the nearest of the boss and the party is inside the view's edge;
     * under nought when something is out of it. */
    f32 margin() const { return m_margin; }
    /** An angle brought within a half turn either way. */
    static f32 wrapAngle(f32 angle);

private:
    Vec3 lookPoint(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                   const BossCameraInfo& record) const;
    f32 wantedYaw(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                  const BossCameraInfo& record) const;
    f32 viewMargin(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                   const CameraView& view) const;
    void place();

    WorldCamera m_camera;
    Vec3 m_attention{0.0f, 0.0f, 0.0f};
    f32 m_distance = 0.0f;
    f32 m_margin = 0.0f;
    f32 m_stepOwed = 0.0f; ///< seconds not yet stepped
};

} // namespace gdl::game
