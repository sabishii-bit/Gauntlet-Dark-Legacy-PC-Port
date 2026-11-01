#pragma once

#include "engine/world/WorldCamera.h"

#include "expansions/n64/formats/LegendsN64Camera.h"
#include "game/camera/TowerCamera.h"

namespace gdl::game {

/** Native Legends entry and follow camera. Consumes actor feet, not a pre-offset
 * follow point, and retains individual party bounds for boss-arena fitting. */
class LegendsCamera {
public:
    enum class Mode { Markers, TowerInner, TowerMiddle, TowerOuter };

    void reset(formats::LegendsN64CameraData data, const Vec3& feet, bool opening = true);
    void update(const Vec3& feet, f32 seconds);
    // Boss framing retains every player's feet/head instead of fitting only
    // their mean. Ordinary marker/Tower behavior remains on the original path.
    void reset(formats::LegendsN64CameraData data, std::span<const CameraSubject> party,
               bool opening = true);
    void update(std::span<const CameraSubject> party, f32 seconds);
    void trackBoss(std::optional<Vec3> focus, f32 radius);
    const WorldCamera& camera() const { return m_camera; }
    /** Rendering-only native 30 Hz interpolation; extraSeconds is the caller's
     * remaining fractional simulation step. Never feed this into movement. */
    WorldCamera sample(f32 extraSeconds = 0) const;
    const Vec3& attention() const { return m_attention; }
    f32 distance() const { return m_distance; }
    Mode mode() const { return m_mode; }
    s32 marker() const { return m_marker; }
    /** The native level-entry pan is still running. Tower entry uses its radial
     * camera instead. This reports camera state, not an input/pause policy. */
    bool introActive() const { return m_introActive; }

private:
    /** Native e007cdf0/e007d774's acceleration, cruise and braking track.
     * Retargeting preserves speed; position and look-at are independent tracks. */
    struct EntryTrack {
        Vec3 value{0};
        Vec3 goal{0};
        Vec3 remaining{0};
        f32 speed = 0;
        f32 peak = 0;
        f32 acceleration = 0;
        f32 elapsed = 0;
        s32 phase = 3;
        bool angular = false;

        void retarget(Vec3 destination);
        void advance(f32 seconds);
        bool done() const { return phase == 3; }
    };

    void tick(const Vec3& feet);
    void resetImpl(formats::LegendsN64CameraData data, std::span<const CameraSubject> party,
                   bool opening);
    void advanceIntro(const Vec3& feet, f32 seconds);
    Vec3 target(const Vec3& feet, f32& pitch, f32& yaw);
    void place();
    Vec3 partyTarget(const Vec3& feet) const;
    f32 followDistance() const;

    formats::LegendsN64CameraData m_data;
    WorldCamera m_camera;
    WorldCamera m_previousCamera;
    Vec3 m_attention{0};
    Vec3 m_smoothed{0};
    Vec3 m_velocity{0};
    Vec3 m_previousTarget{0};
    f32 m_distance = 36;
    f32 m_distanceVelocity = 0;
    f32 m_yawVelocity = 0;
    f32 m_pitchVelocity = 0;
    f32 m_accumulator = 0;
    Mode m_mode = Mode::Markers;
    s32 m_marker = -1;
    bool m_transition = false;
    EntryTrack m_introPosition;
    EntryTrack m_introFocus;
    EntryTrack m_introAngles;
    Vec3 m_introFocusOffset{0};
    bool m_introActive = false;
    std::array<CameraSubject, 4> m_party{};
    usize m_partySize = 1;
    bool m_bossArena = false;
    f32 m_bossRadius = 8;
};

} // namespace gdl::game
