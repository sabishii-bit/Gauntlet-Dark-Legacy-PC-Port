#pragma once

#include <optional>

#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

#include "expansions/n64/camera/LegendsCameraChannel.h"

namespace gdl::game {

/**
 * The channel camera game.bin's scripts drive, the Tower ceremonies among them: a centre, an
 * orbit about it in angles and distance, a point to look at and a rotation, each a
 * LegendsCameraChannel that the setters cut or move. Only step() (e007dd70) advances them and
 * builds the view from them (e007db00), so a setter shows on the next step. The dolly channel
 * (e00eb0f0), which no Tower script turns on, is not kept.
 */
class LegendsCameraRig {
public:
    using Times = LegendsCameraChannel::Times;

    /** e007cfd0(0, ...): the times every channel plans with; those that are on plan again. */
    void setTimes(const Times& times);
    /** e007de28: the centre cut to `view`'s eye and the rotation to its angles, both on, and the
     * orbit, distance and attention off. */
    void settle(const WorldCamera& view);
    /** e007d1d4: the point the orbit turns about. */
    void setCentre(const Vec3& point, bool cut);
    /** e007d53c: the eye `distance` back from the centre along WorldCamera's (pitch, yaw),
     * which e007d2c0 keeps as (-pitch, yaw, 0) and e007d394 as (0, 0, distance). */
    void setOrbit(f32 distance, f32 pitch, f32 yaw, bool cut);
    /** e007d584: the point the view looks at; none turns the channel off. */
    void setAttention(const std::optional<Vec3>& point, bool cut);
    /** e007d67c: the view's turn, as WorldCamera's pitch, yaw and roll, while it has no point to
     * look at; none turns the channel off. */
    void setRotation(const std::optional<Vec3>& angles, bool cut);
    /** e007df20: back to the view from `eye` along (pitch, yaw), about the centre's goal. The
     * rotation takes over at the angles shown and turns to (pitch, yaw) as the attention goes
     * off; the orbit and distance carry the eye back to `eye`. */
    void returnTo(const Vec3& eye, f32 pitch, f32 yaw);
    /** e007dd70: advances every channel by `seconds`, then builds the view. True once every
     * channel rests. */
    bool step(f32 seconds);
    /** The view the last step built. */
    const WorldCamera& view() const { return m_view; }
    /** The attention channel's point while it is on. */
    std::optional<Vec3> attention() const;

private:
    WorldCamera build() const;

    Times m_times;
    LegendsCameraChannel m_centre;         ///< e013c0d8
    LegendsCameraChannel m_orbit{true};    ///< e0133d48, (-pitch, yaw, 0)
    LegendsCameraChannel m_distance;       ///< e0133ae0, (0, 0, distance)
    LegendsCameraChannel m_attention;      ///< e00fe178
    LegendsCameraChannel m_rotation{true}; ///< e00f8850, e0014d04's (-pitch, yaw, -roll)
    WorldCamera m_view;
};

} // namespace gdl::game
