#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "engine/world/WorldCamera.h"

#include "game/netplay/InputCommand.h"

namespace gdl::game {

/** A presentation transform, not a save or a client-owned physics body. A grant
 * identifies the seat occupant. Increment continuity after every teleport or
 * other hard relocation, keeping it on subsequent snapshots so loss cannot
 * turn a cut into movement through walls. Neither identifier may be reused
 * within an epoch. */
struct SeatMotion {
    u32 grant = 0;
    u32 continuity = 0;
    Vec3 position{0};
    f32 yaw = 0;
};

/** Full host-authored movement state at the END of tick. Empty seats mean absent,
 * not unchanged. Full states recover after loss without a delta baseline. This
 * is the movement/camera slice only: no AI, combat, animation, inventory or save
 * replication is implied. The host alone runs the shared camera and its bounds. */
struct MotionSnapshot {
    u64 epoch = 0;
    u64 tick = 0;
    u32 cameraContinuity = 0;
    WorldCamera camera;
    // Host reference projection retained in the packet format for diagnostics and
    // compatibility. Replica rendering uses local FoV/window settings, not these values.
    f32 horizontalFov = 1.0471976f;
    f32 aspect = 640.0f / 448.0f;
    std::array<std::optional<SeatMotion>, InputCommand::kSeats> players;

    bool valid() const;
};

/** Bounded little-endian datagram. Seats are indexed by a four-bit presence mask,
 * never vector order. Does not authenticate its sender; the receiving session
 * must bind it to the admitted host connection. */
class MotionPacket {
public:
    static constexpr u16 kVersion = 1;
    static constexpr usize kHeaderBytes = 60;
    static constexpr usize kSeatBytes = 24;
    static constexpr usize kMaxBytes = kHeaderBytes + InputCommand::kSeats * kSeatBytes;

    static std::optional<std::vector<u8>> encode(const MotionSnapshot& snapshot);
    static std::optional<MotionSnapshot> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
