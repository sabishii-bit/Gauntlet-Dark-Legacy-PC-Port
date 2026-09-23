#pragma once

#include <span>

#include "engine/assets/WorldData.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

#include "game/world/TowerCamera.h"

namespace gdl::game {

/** The boss as the camera sees it: where it stands, which way it faces, how big it is, and
 * whether the fight is on. */
struct BossCameraSubject {
    Vec3 position{0.0f, 0.0f, 0.0f};
    float facing = 0.0f;
    float radius = 4.0f;
    float height = 4.0f; ///< of its body's centre, which is kept in view
    bool awake = false;
    Vec3 attentionOffset{0.0f}; ///< root and vertical drift above the floor position
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
    static constexpr float kFrameRate = 30.0f;  ///< the original's steps are per frame
    static constexpr float kOutStep = 10.0f;    ///< units back a frame when something is off screen
    static constexpr float kTightMargin = 2.0f; ///< within this of the edge it backs off
    static constexpr float kWantedMargin = 2.5f; ///< the slack it settles at
    static constexpr float kLooseMargin = 4.0f;  ///< past this it comes in
    static constexpr float kFarthest = 2.0f;     ///< of the record's greatest distance
    static constexpr float kTurnRate = 2.0f;     ///< radians a second the yaw may swing
    static constexpr float kEase = 6.0f;         ///< how quickly the look point follows

    /** Stands the camera up at once, as the level starts. */
    void reset(const BossCameraSubject& boss, std::span<const CameraSubject> party,
               const BossCameraInfo& record, const CameraView& view);
    void update(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                const BossCameraInfo& record, const CameraView& view, float seconds);

    const WorldCamera& camera() const { return m_camera; }
    float yaw() const { return m_camera.yaw; }
    float pitch() const { return m_camera.pitch; }
    float distance() const { return m_distance; }
    const Vec3& attention() const { return m_attention; }
    /** How far, in units, the nearest of the boss and the party is inside the view's edge;
     * under nought when something is out of it. */
    float margin() const { return m_margin; }
    /** An angle brought within a half turn either way. */
    static float wrapAngle(float angle);

private:
    Vec3 lookPoint(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                   const BossCameraInfo& record) const;
    float wantedYaw(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                    const BossCameraInfo& record) const;
    float viewMargin(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                     const CameraView& view) const;
    void place();

    WorldCamera m_camera;
    Vec3 m_attention{0.0f, 0.0f, 0.0f};
    float m_distance = 0.0f;
    float m_margin = 0.0f;
    float m_stepOwed = 0.0f; ///< seconds not yet stepped
};

} // namespace gdl::game
