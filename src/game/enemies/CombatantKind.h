#pragma once

#include "engine/core/Types.h"

namespace gdl::game {
/** CRITTER descriptor families, distinct from level enemy/boss encounter ids. */
// NOLINTNEXTLINE(performance-enum-size): Keep imported signed IDs intact.
enum class CombatantKind : s32 { Unknown = 0, Golem = 3, Boss = 4, Gargoyle = 7, General = 8 };
} // namespace gdl::game
