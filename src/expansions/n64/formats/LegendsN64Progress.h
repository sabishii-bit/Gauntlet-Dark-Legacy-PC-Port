#pragma once

#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

/** Native contact-activated type9 traps, not ordinary PPower pickups. */
struct LegendsN64Obelisk {
    u32 nativeIndex = 0;
    u16 templateKey = 0;
    u16 flags = 0;
    u32 activationFlags = 0;
    u32 identity = 0; ///< native one-based IPR parameter, distinct from template subtype
    u32 sound = 0;
    u8 placementFlags = 0;
    u8 minimumPlayers = 0;
    Vec3 position{0};
    f32 yaw = 0;
    f32 scale = 1;
    f32 radius = 0;
    f32 height = 0;
    std::string idleSequence;
    std::string spentSequence;
    bool groundProjected = true;
    bool supported = false;
};

struct LegendsN64Obelisks {
    std::vector<LegendsN64Obelisk> obelisks;
    std::vector<std::string> warnings;
};

/** Decode the native trap table and preserve unsupported type9 records for
 * diagnostics. Placement coordinates precede native floor projection. */
LegendsN64Obelisks decodeLegendsN64Obelisks(std::span<const u8> placements,
                                            std::span<const u8> traps);

} // namespace gdl::formats
