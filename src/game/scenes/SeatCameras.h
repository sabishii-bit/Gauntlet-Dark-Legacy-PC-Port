#pragma once

#include <array>
#include <span>

#include "engine/core/Types.h"

#include "game/camera/TowerCamera.h"
#include "game/party/PlayerRuntime.h"

namespace gdl {
class NativeStateDigest;
struct WorldLocator;
} // namespace gdl

namespace gdl::game {

/**
 * The seats' own follow cameras, for the netplay room's solo-camera rule: one camera a seat,
 * each kept on its own standing character and a fallen one's kept with the party, and the
 * seat this machine looks through. The shared camera and the choice of which camera shows
 * stay with the scene.
 */
class SeatCameras {
public:
    static constexpr usize kSeats = 4;
    using States = std::array<TowerCamera::State, kSeats>;

    /** Stands every seat's camera up on what it follows, as a level start does. */
    void reset(std::span<const PlayerRuntime> players, std::span<const CameraSubject> subjects,
               std::span<const WorldLocator> markers, const CameraRange& range,
               const CameraView& view);
    /** Follows for `seconds`. */
    void follow(std::span<const PlayerRuntime> players, std::span<const CameraSubject> subjects,
                std::span<const WorldLocator> markers, const CameraRange& range,
                const CameraView& view, f32 seconds);
    /** Ends a trigger camera's hold on the party's current target, without re-aiming. */
    void snap(std::span<const PlayerRuntime> players, std::span<const CameraSubject> subjects,
              const CameraRange& range);

    /** The camera of `seat`, clamped into the seats. */
    const TowerCamera& camera(s32 seat) const;
    /** The seat shown here: the view seat while it is in the party, else the lowest seat
     * standing; none (-1) with nobody. */
    s32 presented(std::span<const PlayerRuntime> players) const;
    /** Presentation only: below nought, the lowest standing seat is shown. */
    void setViewSeat(s32 seat) { m_viewSeat = seat; }
    s32 viewSeat() const { return m_viewSeat; }

    /** The rewind seam: the four cameras' values, seat by seat. */
    States states() const;
    void restore(const States& states);
    /** Every seat's camera in turn, never stopping early; false when any refused. */
    bool appendNativeDigest(NativeStateDigest& digest) const;

private:
    /** Hands each party seat's camera the subjects it follows: its own body while standing,
     * else the standing party's (all of them with nobody standing). */
    template <class Apply>
    void eachSeat(std::span<const PlayerRuntime> players, std::span<const CameraSubject> subjects,
                  Apply apply);

    std::array<TowerCamera, kSeats> m_cameras;
    s32 m_viewSeat = -1;
};

} // namespace gdl::game
