#pragma once

#include <optional>
#include <string>

#include "engine/core/Types.h"

#include "formats/WorldFile.h"
#include "game/level/ImportedBoss.h"

namespace gdl::game {

/** The boss's safe-rock roster: CritterProcessSafeRocks 8c08aa00 collects at most this many
 * type 10 subtype 41 items through CollectSafeRocks 8c0a9ce0. */
inline constexpr usize kMostDreamcastArenaRocks = 16;

/** A type 10 subtype 41 safe rock as a Dreamcast arena stands it. */
struct LegendsDreamcastArenaRock {
    ImportedBoss::ArenaRock body; ///< tierHealth is the native health of one segment
    s32 tier = 0;                 ///< the segments placed (params +2), from 0 rubble to 3
    s32 armor = 0;
    bool dormant = false; ///< placement flag 2: unseen until the arena raises it
};

/** The safe rock a placement stands while `standing` rocks already do. Nothing for a
 * placement that is no safe rock; nothing with a diagnostic for a safe rock the arena
 * cannot stand: an unsupported segment count, durability or body, or one past the roster. */
std::optional<LegendsDreamcastArenaRock>
legendsDreamcastArenaRock(const formats::ItemInfoRecord& info,
                          const formats::ItemInstanceRecord& instance, usize standing,
                          std::string* diagnostic = nullptr);

} // namespace gdl::game
