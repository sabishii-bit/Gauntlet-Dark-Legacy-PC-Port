#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "expansions/shared/formats/LegendsMap.h"
#include "game/level/ImportedActors.h"

namespace gdl::game {

struct LegendsActorRoster {
    std::vector<ImportedActor> actors;
    std::vector<std::string> warnings;
};

/** Decodes authored IPR actor records, PGEn templates and the native executable's
 * realm/level roster. Repeating generators are deliberately not returned here.
 * Unsupported species/activation rules are diagnosed rather than substituted. */
LegendsActorRoster
decodeLegendsN64Actors(std::span<const u8> placements, std::span<const u8> templates,
                       std::span<const u8> program, std::string_view levelName,
                       std::span<const formats::LegendsMapCollisionTriangle> collision);

/** Native proximity-only activation, frozen by Stop Time. Callers supply only
 * living participants (EnemyView has no alive field), at actual feet positions.
 * Visibility, invisibility, and attack contact do not wake these placements. */
bool legendsActorReady(const ImportedActor& actor, std::span<const EnemyView> players,
                       bool timeStopped = false);

} // namespace gdl::game
