#pragma once

#include "engine/core/Types.h"

namespace gdl {
/** Shared interpretation of object-layer rendering flags, separate from world,
 * particle and bitmap flags. Other bits remain with their original records. */
struct ObjectMaterial {
    bool additive = false;
    bool depthWrite = true;
    bool depthTest = true;
    bool chrome = false;
    bool sorted = false;
    u32 facing = 0;
    f32 sortBias = 0;

    static constexpr ObjectMaterial fromFlags(u32 flags) {
        f32 bias = 0.0f;
        if ((flags & 0x400000U) != 0) {
            bias = -20000.0f;
        } else if ((flags & 0x80000U) != 0) {
            bias = -10000.0f;
        }
        return {.additive = (flags & 0x800000U) != 0,
                .depthWrite = (flags & 0x80U) == 0,
                .depthTest = (flags & 0x40U) == 0,
                .chrome = (flags & 0x8000U) != 0,
                .sorted = (flags & 0x800U) != 0,
                .facing = (flags >> 24U) & 0xFU,
                .sortBias = bias};
    }
};
} // namespace gdl
