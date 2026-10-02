#pragma once

#include <span>

#include "engine/core/Types.h"

namespace gdl::game {

struct ClassStats;

/** Decodes a native PDATA WAD directly to runtime tuning; throws on malformed tables. */
ClassStats parseNativeClassStats(std::span<const u8> bytes);

} // namespace gdl::game
