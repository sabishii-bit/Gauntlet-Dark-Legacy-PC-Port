#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

/** Native damaging traps, not switches, locked gates or explosives. */
struct LegendsN64Trap {
    u32 nativeIndex = 0;
    u16 templateKey = 0;
    u16 flags = 0;
    u8 placementFlags = 0;
    u8 minimumPlayers = 0;
    Vec3 position{0};
    f32 yaw = 0;
    f32 scale = 1;
    bool groundProjected = true;
    u32 shape = 5;
    Vec3 dimensions{0}; ///< cylinder radius/height or full box dimensions
    u16 restFrames = 0;
    u8 activeFrames = 0; ///< zero obtains the duration from the authored sequence
    u8 damage = 0;
    u32 sound = 0;
    std::string idleSequence, activeSequence;
    std::string contactNode; ///< type10 attacks follow this exact named tree attachment
};

struct LegendsN64Traps {
    std::vector<LegendsN64Trap> traps;
    std::vector<std::string> warnings;
};

LegendsN64Traps decodeLegendsN64Traps(std::span<const u8> placements,
                                      std::span<const u8> templates);

/** e004161c's native stage difficulty, used by trap damage and recovery delay.
 * Unknown levels return no scale rather than borrowing another realm's rules. */
std::optional<f32> legendsN64TrapScale(std::span<const u8> program, std::string_view levelName);

} // namespace gdl::formats
