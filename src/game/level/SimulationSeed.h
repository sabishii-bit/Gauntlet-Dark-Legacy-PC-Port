#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {
/** Stage-name seed for native simulation: 32-bit FNV-1a over the exact unsigned
 * bytes, with arithmetic modulo 2^32. Case and embedded zeroes are significant.
 * Unlike std::hash, this definition does not depend on the standard library or
 * size_t width. It is a reproducible initial seed, not a resource fingerprint. */
constexpr u32 simulationSeed(std::string_view stage) {
    u32 seed = 2166136261U;
    for (const char byte : stage) {
        seed = (seed ^ static_cast<u8>(byte)) * 16777619U;
    }
    return seed;
}
} // namespace gdl::game
