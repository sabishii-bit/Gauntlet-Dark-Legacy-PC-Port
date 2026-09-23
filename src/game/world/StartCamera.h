#pragma once

#include <cstdint>

#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

namespace gdl::game {

/**
 * The camera a level starts on, scripted as the original scripts it: it holds at the level's
 * start marker, looking the marker's way as far off as the party stands, for a while that a
 * button may cut short once most of it has passed; then it rides to the follow camera, its
 * position and the point it looks at each moving at the pace a tick (from far off, faster)
 * until both have all but arrived, when the follow camera takes over. The clock runs in
 * ticks of real time, so the ride takes as long at any frame rate.
 */
class StartCamera {
public:
    static constexpr std::int32_t kHoldTicks = 91;  ///< the original's timer
    static constexpr std::int32_t kSkipBelow = 45;  ///< a button ends the hold from here down
    static constexpr float kUnitsPerTick = 0.25f;   ///< the ride's pace: a glide, not a snap
    static constexpr float kPositionReach = 200.0f; ///< beyond these the pace grows with the gap
    static constexpr float kAttentionReach = 20.0f;
    static constexpr float kArrival = 0.3f; ///< within this of the follow camera is there

    enum class Phase : std::uint8_t { Off, Hold, Ride };

    /** Starts the hold at `marker`, looking as far along its facing as `party` lies from it. */
    void start(const WorldCamera& marker, const Vec3& party);
    void stop() { m_phase = Phase::Off; }
    /** Advances `ticks`, `skip` ending a hold that has run long enough, riding toward the
     * follow camera's `position` and `attention`; false once it has arrived. */
    bool update(std::int32_t ticks, bool skip, const Vec3& position, const Vec3& attention);

    bool active() const { return m_phase != Phase::Off; }
    Phase phase() const { return m_phase; }
    std::int32_t ticksLeft() const { return m_ticks; }
    const WorldCamera& camera() const { return m_camera; }
    const Vec3& attention() const { return m_attention; }

private:
    /** Moves `point` at the pace toward `target`, or by the gap's share of `reach` when it is
     * farther than that; true when it was already within kArrival. */
    static bool approach(Vec3& point, const Vec3& target, float reach, std::int32_t ticks);
    /** Turns the camera to look at its attention. */
    void look();

    Phase m_phase = Phase::Off;
    std::int32_t m_ticks = 0;
    WorldCamera m_camera;
    Vec3 m_attention{0.0f, 0.0f, 0.0f};
};

} // namespace gdl::game
