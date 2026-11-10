#pragma once

#include <optional>
#include <string>

#include "engine/core/Types.h"

#include "formats/WorldFile.h"
#include "game/level/ImportedBoss.h"

namespace gdl::game {

/** The boss's safe-rock roster: CritterProcessSafeRocks 8c08aa00 collects at most this many
 * type 10 subtype 41 items through CollectSafeRocks 8c0a9ce0, leaving the rest ordinary items. */
inline constexpr usize kMostDreamcastArenaRocks = 16;

/** A type 10 subtype 41 safe rock as a Dreamcast arena stands it. */
struct LegendsDreamcastArenaRock {
    ImportedBoss::ArenaRock body; ///< tierHealth is the native health of one segment
    s32 tier = 0;                 ///< the segments placed (params +2), from 0 rubble to 3
    s32 armor = 0;
    bool dormant = false; ///< placement flag 2: unseen until the arena raises it
};

/** What a Dreamcast arena makes of a placement. */
struct LegendsDreamcastArenaAdmission {
    std::optional<LegendsDreamcastArenaRock> rock; ///< the cover it stands, when it stands any
    /** Why a safe rock stands no cover: an unsupported segment count, durability or body, or a
     * place past the roster. Empty for cover and for what is no safe rock. */
    std::string diagnostic;
    /** Left to the ordinary fixtures: what is no safe rock, and a safe rock past the roster. */
    bool ordinary = false;
};

/** What a placement is to the arena while `standing` safe rocks already stand. */
LegendsDreamcastArenaAdmission
legendsDreamcastArenaRock(const formats::ItemInfoRecord& info,
                          const formats::ItemInstanceRecord& instance, usize standing);

} // namespace gdl::game
