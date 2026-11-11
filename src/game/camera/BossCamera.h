#pragma once

#include <numbers>
#include <optional>
#include <span>
#include <utility>

#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

#include "game/camera/TowerCamera.h"

namespace gdl::game {

/** The boss as the camera sees it: where it stands, which way it faces, how big it is, and
 * whether the fight is on. */
struct BossCameraSubject {
    enum class Focus : u8 { Combat, Shard, Wizard };
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 facing = 0.0f;
    f32 radius = 4.0f;
    f32 height = 4.0f;          ///< of its body's centre, which is kept in view
    bool engaged = false;       ///< the encounter has begun (BossView::engaged), framed as a fight
    Vec3 attentionOffset{0.0f}; ///< root and vertical drift above the floor position
    std::optional<Vec3> baseAttention; ///< initialized model root, without vertical drift
    Focus focus = Focus::Combat;
};

/**
 * The camera of a boss fight, by the level's boss camera record: it looks at the boss (a
 * little off it, nearer its feet the further back it stands) from behind the party, along
 * the party's line to the boss, swung no further about the boss's facing than the record
 * allows; it looks down at the record's pitch, steeper the closer it is; and it backs off
 * or comes in, within the record's distances, so that the boss and every player stay in
 * view. Before the encounter begins it follows the party alone from the level camera
 * markers, at the record's player distances.
 *
 * Nothing it does is sudden: the look point, the yaw, the distance and the pitch each
 * move toward where they are wanted under a speed and an acceleration (LimitCamVal2), as
 * the original's do, so that the view neither jumps nor shakes as the party moves about.
 */
class BossCamera {
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const BossCamera& value) {
        const auto& [camera, attention, lookBase, lookDirection, lookTurnVelocity, lookLength,
                     lookVelocity, distance, distanceVelocity, yawVelocity, pitchVelocity, margin,
                     band, marker] = value;
        visitor.values(camera, attention, lookBase, lookDirection, lookTurnVelocity, lookLength,
                       lookVelocity, distance, distanceVelocity, yawVelocity, pitchVelocity, margin,
                       band, marker);
    }

public:
    bool operator==(const BossCamera&) const = default;

    static constexpr f32 kFrameRate = 30.0f;  ///< the original's steps are per frame
    static constexpr f32 kOutStep = 10.0f;    ///< units back a frame when something is off screen
    static constexpr f32 kTightMargin = 2.0f; ///< within this of the edge it backs off
    static constexpr f32 kHeldTightMargin = 2.25f; ///< and, once backing off, keeps on to this
    static constexpr f32 kWantedMargin = 2.5f;     ///< the slack it settles at
    static constexpr f32 kLooseMargin = 4.0f;      ///< past this it comes in
    static constexpr f32 kFarthest = 2.0f;         ///< of the record's greatest distance
    static constexpr f32 kWizardReach = 2.0f; ///< the greatest distance, while the wizard stands
    static constexpr f32 kYawSpeed = std::numbers::pi_v<f32> / 2.0f; ///< radians a second
    static constexpr f32 kYawAcceleration = std::numbers::pi_v<f32> / 6.0f;
    static constexpr f32 kOutSpeed = 200.0f; ///< units a second it may back off
    static constexpr f32 kInSpeed = 50.0f;   ///< and come in
    static constexpr f32 kDistanceAcceleration = 75.0f;
    static constexpr f32 kPitchSpeed = std::numbers::pi_v<f32> / 2.0f;
    static constexpr f32 kPitchAcceleration = std::numbers::pi_v<f32> / 12.0f;
    static constexpr f32 kStopScale = 0.1f; ///< of the speed, under which a move is done
    static constexpr f32 kPitchStopScale = kStopScale;
    static constexpr f32 kPitchHold = 10.0f;  ///< the pitch waits while the distance is further off
    static constexpr f32 kLookSpeed = 200.0f; ///< units a second the look point may move
    static constexpr f32 kLookAcceleration = 75.0f;
    static constexpr f32 kLookTurnSpeed = 20.0f; ///< its direction's components, a second
    static constexpr f32 kLookTurnAcceleration = 20.0f;
    static constexpr f32 kLookTurnReset = 0.965f; ///< cosine under which the direction restarts

    /** Optional presentation policy for a platform's pre-combat camera. Values
     * are supplied per call, never retained in rewind state. The default is the
     * Dark Legacy approach; combat framing is unaffected by this policy. */
    struct ApproachPolicy {
        f32 maxDistanceScale = 1.0f;
        f32 inwardLooseMargin = kLooseMargin;
        f32 inwardHeldMargin = kWantedMargin;
        f32 inwardGain = 1.0f;
        bool boundsMidpoint = false;
        std::optional<std::pair<Vec3, Vec3>> attentionBounds;
    };

    /** Stands the camera up at once, as the level starts. */
    void reset(const BossCameraSubject& boss, std::span<const CameraSubject> party,
               const BossCameraInfo& record, const CameraView& view,
               std::span<const WorldLocator> markers = {},
               const ApproachPolicy* approach = nullptr);
    void update(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                const BossCameraInfo& record, const CameraView& view, f32 seconds,
                std::span<const WorldLocator> markers = {},
                const ApproachPolicy* approach = nullptr);

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
    /** A value moved toward `target` for `seconds` under a speed (`least` to `most`) and an
     * acceleration, braking so as to stop on it, and done once within `stopScale` of the
     * speed; `velocity` carries between calls. LimitCamVal2. */
    static f32 limit(f32 value, f32 target, f32 least, f32 most, f32 acceleration, f32 stopScale,
                     f32& velocity, bool wrap, f32 seconds);

private:
    Vec3 lookPoint(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                   const BossCameraInfo& record, const ApproachPolicy* approach) const;
    f32 wantedYaw(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                  const BossCameraInfo& record) const;
    f32 wantedDistance(f32 nearest, f32 farthest, const ApproachPolicy* approach);
    f32 viewMargin(const BossCameraSubject& boss, std::span<const CameraSubject> party,
                   const CameraView& view) const;
    void followLook(const Vec3& target, f32 seconds);
    void place();
    const WorldLocator* approachMarker(std::span<const WorldLocator> markers, const Vec3& near);

    WorldCamera m_camera;
    Vec3 m_attention{0.0f, 0.0f, 0.0f};
    Vec3 m_lookBase{0.0f};      ///< where the look point set out from
    Vec3 m_lookDirection{0.0f}; ///< which way it is going, eased by component
    Vec3 m_lookTurnVelocity{0.0f};
    f32 m_lookLength = 0.0f; ///< how far along it is
    f32 m_lookVelocity = 0.0f;
    f32 m_distance = 0.0f;
    f32 m_distanceVelocity = 0.0f;
    f32 m_yawVelocity = 0.0f;
    f32 m_pitchVelocity = 0.0f;
    f32 m_margin = 0.0f;
    u32 m_band = 0; ///< whether it was backing off (0x200) or coming in (0x100)
    s32 m_marker = -1;
};

} // namespace gdl::game
