#pragma once

#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

/** A native type2 trap; its target is a WOBJ name, not a model/archive name. */
struct LegendsN64Switch {
    u32 nativeIndex = 0;
    u16 templateKey = 0;
    u16 flags = 0;
    u32 activationFlags = 0;
    u8 placementFlags = 0;
    u8 minimumPlayers = 0;
    std::string target;
    std::string idleSequence;
    std::string activeSequence;
    Vec3 position{0};
    f32 yaw = 0;
    f32 scale = 1;
    f32 radius = .5f;
    f32 height = 1;
    s16 firstEndpoint = 0; ///< native authored tenths; retained without rounding
    s16 secondEndpoint = 0;
    u32 sound = 0;
    bool groundProjected = true; ///< native floor owner carries the plate with moving scenery
    bool supported = false;
    u16 mode() const { return flags & 0xf00U; }
    bool oneShot() const { return (flags & 0x1000U) != 0; }
    bool hidden() const { return (placementFlags & 8U) != 0; }
};

struct LegendsN64Switches {
    std::vector<LegendsN64Switch> switches;
    std::vector<std::string> warnings;
};

/** Preserve every type2 record, including unsupported modes for diagnostics.
 * Malformed tables, missing templates and unterminated target names throw FormatError. */
LegendsN64Switches decodeLegendsN64Switches(std::span<const u8> placements,
                                            std::span<const u8> traps);

} // namespace gdl::formats
