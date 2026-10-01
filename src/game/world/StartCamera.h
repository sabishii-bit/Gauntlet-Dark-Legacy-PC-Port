#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {

/** Level-entry hold and handoff. Normal levels use angular/radius interpolation;
 * special/boss cameras retain their separate legacy path. */
class StartCamera {
public:
    static constexpr s32 kHoldTicks = 180; ///< three seconds on the 60 Hz tick clock
    static constexpr s32 kRideTicks = 20;  ///< ten 30 Hz camera updates
    static constexpr s32 kWarmSteps = 9;   ///< follow-history convergence at handoff
    static constexpr s32 kLegacyHoldTicks = 91;
    static constexpr s32 kSkipBelow = 45;         ///< a button ends the hold from here down
    static constexpr f32 kUnitsPerTick = 0.25f;   ///< the ride's pace: a glide, not a snap
    static constexpr f32 kPositionReach = 200.0f; ///< beyond these the pace grows with the gap
    static constexpr f32 kAttentionReach = 20.0f;
    static constexpr f32 kArrival = 0.3f; ///< within this of the follow camera is there

    enum class Phase : u8 { Off, Hold, Ride };
    enum class Mode : u8 { Standard, Legacy };

    /** Holds at the marker; standard entrances use its facing, boss/legacy
     * entrances aim at the supplied party focus instead. */
    void start(const WorldCamera& marker, const Vec3& party, Mode mode = Mode::Standard);
    void stop() { m_phase = Phase::Off; }
    /** Advances toward the follow camera's position/attention; false when arrived.
     * Only the legacy hold accepts a button skip. */
    bool update(s32 ticks, bool skip, const Vec3& position, const Vec3& attention);

    bool active() const { return m_phase != Phase::Off; }
    Phase phase() const { return m_phase; }
    Mode mode() const { return m_mode; }
    s32 ticksLeft() const { return m_ticks; }
    const WorldCamera& camera() const { return m_camera; }
    const Vec3& attention() const { return m_attention; }

private:
    /** Moves `point` at the pace toward `target`, or by the gap's share of `reach` when it is
     * farther than that; true when it was already within kArrival. */
    static bool approach(Vec3& point, const Vec3& target, f32 reach, s32 ticks);
    /** Turns the camera to look at its attention. */
    void look();
    bool standard(s32 ticks, const Vec3& position, const Vec3& attention);

    Phase m_phase = Phase::Off;
    Mode m_mode = Mode::Standard;
    s32 m_ticks = 0;
    s32 m_rideTicks = 0;
    WorldCamera m_from;
    Vec3 m_fromAttention{0};
    Vec3 m_toAttention{0};
    f32 m_fromDistance = 0;
    f32 m_toDistance = 0;
    f32 m_yawDelta = 0;
    f32 m_pitchDelta = 0;
    WorldCamera m_camera;
    Vec3 m_attention{0.0f, 0.0f, 0.0f};
};

} // namespace gdl::game
