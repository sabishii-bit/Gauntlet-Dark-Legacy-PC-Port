#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "game/level/ImportedBoss.h"

namespace gdl::game {

/** A Dreamcast arena's native identity and its engine combat adapter. Archive
 * names select Dreamcast resources; counterpart names only select GDL mechanics. */
struct LegendsDreamcastBossEncounter {
    ImportedBoss combat;
    std::string map;
    s32 nativeKind = -1;
    s32 nativeRealm = -1;
    std::string archive;
};

/** Reads the Dreamcast WRLD/LEVL records. Ordinary, unknown and non-shipped boss
 * maps return no encounter; malformed tables throw FormatError. No N64 assets
 * or GameCube campaign progress are consulted. */
std::optional<LegendsDreamcastBossEncounter> legendsDreamcastBoss(std::span<const u8> worlddata,
                                                                  std::span<const u8> worlds,
                                                                  std::string_view levelName);

} // namespace gdl::game
