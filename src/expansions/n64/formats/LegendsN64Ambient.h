#pragma once

#include <span>
#include <vector>

#include "engine/math/Math.h"

namespace gdl::formats {

struct LegendsN64Ambient {
    u32 nativeIndex = 0;
    Vec3 position{0};
    u16 token = 0xffff;
    f32 radius = 0;
    u8 kind = 0;
    u8 subtype = 0;
};

/** Native IPR sound placements, independent of meshes and guessed sound names. */
std::vector<LegendsN64Ambient> decodeLegendsN64Ambient(std::span<const u8> placements);

} // namespace gdl::formats
