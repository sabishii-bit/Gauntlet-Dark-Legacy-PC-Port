#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

struct LegendsN64GateCollider {
    u32 shape = 5;      ///< native 1=cylinder, 3=box, 5=absent
    Vec3 dimensions{0}; ///< scaled radius/height for cylinders, full XYZ for boxes
    Vec2 offset{0};     ///< unscaled local XZ, rotated by the native placement yaw
};

struct LegendsN64Gate {
    u32 nativeIndex = 0;
    u16 templateKey = 0;
    u16 flags = 0;
    u8 placementFlags = 0;
    u8 minimumPlayers = 0;
    Vec3 position{0};
    f32 yaw = 0; ///< native placement yaw, as used for the TRAP figure
    f32 scale = 1;
    bool groundProjected = true;
    std::string idleSequence;
    std::string openingSequence;
    u32 sound = 0;
    LegendsN64GateCollider primary;
    std::array<LegendsN64GateCollider, 2> frames;
};

/** Type3/4 key doors in IPR/PTrap. Unknown gate behavior is diagnosed, not
 * treated as a free-open door. Secondary frame colliders survive opening. */
struct LegendsN64Gates {
    std::vector<LegendsN64Gate> gates;
    std::vector<std::string> warnings;
};
LegendsN64Gates decodeLegendsN64Gates(std::span<const u8> placements,
                                      std::span<const u8> templates);

} // namespace gdl::formats
