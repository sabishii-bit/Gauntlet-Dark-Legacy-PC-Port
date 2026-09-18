#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {

/**
 * The camera a level starts on, scripted as the original scripts it: it holds at the level's
 * start marker, looking the marker's way as far off as the party stands, for a while that a
 * button may cut short once most of it has passed; then it rides to the follow camera, its
 * position and the point it looks at each moving a unit a tick (from far off, faster) until
 * both have all but arrived, when the follow camera takes over.
 */
class StartCamera {
public:
    static constexpr s32 kHoldTicks = 91;         ///< the original's timer
    static constexpr s32 kSkipBelow = 45;         ///< a button ends the hold from here down
    static constexpr f32 kUnitsPerTick = 1.0f;    ///< the ride's pace
    static constexpr f32 kPositionReach = 200.0f; ///< beyond these the pace grows with the gap
    static constexpr f32 kAttentionReach = 20.0f;
    static constexpr f32 kArrival = 0.3f; ///< within this of the follow camera is there

    enum class Phase : u8 { Off, Hold, Ride };

    /** Starts the hold at `marker`, looking as far along its facing as `party` lies from it. */
    void start(const WorldCamera& marker, const Vec3& party);
    void stop() { m_phase = Phase::Off; }
    /** Advances `ticks`, `skip` ending a hold that has run long enough, riding toward the
     * follow camera's `position` and `attention`; false once it has arrived. */
    bool update(s32 ticks, bool skip, const Vec3& position, const Vec3& attention);

    bool active() const { return m_phase != Phase::Off; }
    Phase phase() const { return m_phase; }
    s32 ticksLeft() const { return m_ticks; }
    const WorldCamera& camera() const { return m_camera; }
    const Vec3& attention() const { return m_attention; }

private:
    /** Moves `point` at the pace toward `target`, or by the gap's share of `reach` when it is
     * farther than that; true when it was already within kArrival. */
    static bool approach(Vec3& point, const Vec3& target, f32 reach, s32 ticks);
    /** Turns the camera to look at its attention. */
    void look();

    Phase m_phase = Phase::Off;
    s32 m_ticks = 0;
    WorldCamera m_camera;
    Vec3 m_attention{0.0f, 0.0f, 0.0f};
};

} // namespace gdl::game
