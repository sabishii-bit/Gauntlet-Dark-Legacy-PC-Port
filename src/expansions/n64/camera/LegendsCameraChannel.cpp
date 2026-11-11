#include "expansions/n64/camera/LegendsCameraChannel.h"

#include <cmath>

namespace gdl::game {
namespace {
/// A move whose every component is under this ends where it stands (0x3a83126f).
constexpr f32 kStill = .001f;
/// Turns past which wrap() first takes the remainder, so its loop stays short.
constexpr f32 kFarTurns = 4 * kPi;

/** e00149d8: the angle in (-pi, pi], a turn at a time. */
f32 wrap(f32 angle) {
    if (std::abs(angle) > kFarTurns) {
        angle = std::remainder(angle, kTwoPi);
    }
    if (kPi < angle) {
        do {
            angle -= kTwoPi;
        } while (kPi < angle);
    }
    if (angle <= -kPi) {
        do {
            angle += kTwoPi;
        } while (angle <= -kPi);
    }
    return angle;
}

/** The length in the native order of sums, ((x^2 + y^2) + z^2). */
f32 length(const Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

bool still(const Vec3& remaining) {
    return glm::all(glm::lessThan(glm::abs(remaining), Vec3{kStill}));
}
} // namespace

void LegendsCameraChannel::set(const Vec3& goal, bool cut, const Times& times) {
    m_goal = goal;
    if (cut || !m_on) {
        m_value = goal;
        m_speed = 0;
    }
    m_on = true;
    plan(times);
}

void LegendsCameraChannel::retarget() {
    // e007d074 takes the remaining before it wraps the goal, the value and the remaining.
    m_remaining = m_goal - m_value;
    if (m_angular) {
        for (s32 axis = 0; axis < 3; ++axis) {
            m_goal[axis] = wrap(m_goal[axis]);
            m_value[axis] = wrap(m_value[axis]);
            m_remaining[axis] = wrap(m_remaining[axis]);
        }
    }
    if (still(m_remaining)) {
        m_speed = 0;
        m_phase = Phase::Done;
        m_goal = m_value;
    }
}

void LegendsCameraChannel::plan(const Times& times) {
    if (!m_on) {
        return;
    }
    m_clock = 0;
    m_times = times;
    retarget();
    // A dropped move whose length is still over a thousandth plans on; its next update ends it.
    const f32 distance = length(m_remaining);
    if (distance <= kStill) {
        m_phase = Phase::Done;
        m_value = m_goal;
        return;
    }
    const f32 ends = m_times.acceleration + m_times.braking;
    f32 cruise = 0;
    if (0 < ends + m_times.cruise) {
        cruise = distance / (ends * .5f + m_times.cruise);
    }
    if (!(0 < cruise)) {
        return;
    }
    m_cruiseSpeed = cruise;
    // The move starts in its first phase that lasts.
    if (0 < m_times.braking) {
        m_phase = Phase::Braking;
        m_fall = -cruise / m_times.braking;
    }
    if (0 < m_times.cruise) {
        m_phase = Phase::Cruising;
    }
    if (0 < m_times.acceleration) {
        m_phase = Phase::Accelerating;
        m_rise = (cruise - m_speed) / m_times.acceleration;
    }
}

bool LegendsCameraChannel::advance(f32 seconds) {
    if (!m_on) {
        return true;
    }
    // The update moves along the remaining as it found it.
    const Vec3 along = m_remaining;
    if (m_phase != Phase::Done && still(along)) {
        m_speed = 0;
        m_phase = Phase::Done;
        m_goal = m_value;
    }
    // A phase ends only when the update outlasts it, and the rest of the update runs in the next.
    f32 rest = seconds;
    f32 moved = 0;
    if (m_phase == Phase::Accelerating) {
        f32 slice = m_times.acceleration - m_clock;
        if (rest <= slice) {
            slice = rest;
            m_clock += rest;
            rest = 0;
        } else {
            rest -= slice;
            m_clock = 0;
            m_phase = Phase::Cruising;
        }
        const f32 gained = m_rise * slice;
        moved = (m_speed + gained * .5f) * slice;
        m_speed += gained;
    }
    if (m_phase == Phase::Cruising) {
        f32 slice = m_times.cruise - m_clock;
        m_speed = m_cruiseSpeed;
        if (rest <= slice) {
            slice = rest;
            m_clock += rest;
            rest = 0;
        } else {
            rest -= slice;
            m_clock = 0;
            m_phase = Phase::Braking;
        }
        moved += m_speed * slice;
    }
    if (m_phase == Phase::Braking) {
        f32 slice = m_times.braking - m_clock;
        if (rest <= slice) {
            slice = rest;
            m_clock += rest;
        } else {
            m_clock = 0;
            m_phase = Phase::Done;
        }
        const f32 lost = m_fall * slice;
        moved += (m_speed + lost * .5f) * slice;
        m_speed += lost;
    }
    if (m_phase == Phase::Done) {
        // Outlasting the braking lands the move on its goal.
        m_speed = 0;
        m_value = m_goal;
        return true;
    }
    if (0 < moved) {
        // A straight step along the channel's own remaining, in its angles for an angular one.
        const Vec3 step = along * (moved / length(along));
        m_value += step;
        m_remaining -= step;
        if (m_angular) {
            for (s32 axis = 0; axis < 3; ++axis) {
                m_value[axis] = wrap(m_value[axis]);
            }
        }
    }
    return false;
}

} // namespace gdl::game
