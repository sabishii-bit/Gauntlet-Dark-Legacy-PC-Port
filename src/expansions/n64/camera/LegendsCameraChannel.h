#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/**
 * One of game.bin's camera channels (0x4c bytes each): a vector carried to its goal through an
 * accelerating, a cruising and a braking phase, each on its own clock. e007cdf0 plans the move
 * and e007d774 makes one update of it; an angular channel keeps its components in (-pi, pi]
 * (e00149d8) and takes the short way round.
 */
class LegendsCameraChannel {
public:
    /** Seconds each phase lasts (+0x38, +0x3c and +0x40); e007e0d0 holds the shared defaults. */
    struct Times {
        f32 acceleration = .25f;
        f32 cruise = 1;
        f32 braking = .25f;
    };
    enum class Phase : u8 { Accelerating, Cruising, Braking, Done };

    explicit LegendsCameraChannel(bool angular = false) : m_angular{angular} {}

    /** A vector's length in game.bin's order of sums, ((x^2 + y^2) + z^2), as the channels plan
     * and step by it and the rig measures its return. */
    static f32 length(const Vec3& value);

    /** The setters' shared part (e007d1d4, e007d2c0, e007d394, e007d584 and e007d67c): the
     * goal, and for a cut or a channel that was off the value too, at rest. The channel turns
     * on and plans with `times`. */
    void set(const Vec3& goal, bool cut, const Times& times);
    /** e007d584 or e007d67c without a vector: the channel stops planning, moving and showing. */
    void turnOff() { m_on = false; }
    /** e007cdf0: a channel that is on plans its move to the goal with `times`, keeping the speed
     * of a move under way. A move every component of which is under a thousandth is dropped
     * where the channel stands (e007d074). */
    void plan(const Times& times);
    /** e007d774: one update of `seconds`. True when the channel is off or comes to rest. */
    bool advance(f32 seconds);

    bool on() const { return m_on; }
    const Vec3& value() const { return m_value; }
    const Vec3& goal() const { return m_goal; }
    f32 speed() const { return m_speed; }
    Phase phase() const { return m_phase; }

private:
    void retarget();

    Vec3 m_value{0};             ///< +0
    Vec3 m_remaining{0};         ///< +0xc
    Vec3 m_goal{0};              ///< +0x18
    f32 m_cruiseSpeed = 0;       ///< +0x24
    f32 m_speed = 0;             ///< +0x28
    f32 m_rise = 0;              ///< +0x2c: speed gained a second while accelerating
    f32 m_fall = 0;              ///< +0x30: speed lost a second while braking
    Times m_times;               ///< +0x38 to +0x40, as the last plan took them
    f32 m_clock = 0;             ///< +0x44: seconds into the phase
    Phase m_phase = Phase::Done; ///< +0x4a
    bool m_angular = false;      ///< flag 1
    bool m_on = false;           ///< flag 2
};

} // namespace gdl::game
