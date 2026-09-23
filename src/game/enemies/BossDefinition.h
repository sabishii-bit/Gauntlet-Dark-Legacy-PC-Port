#pragma once
#include <string_view>

#include "engine/core/Types.h"

#include "game/enemies/CombatantDefinition.h"
namespace gdl::game {
/** Boss identity and combat policy; encounter progression belongs to Bosses. */
CombatantDefinition bossDefinition(std::string_view name);
std::string_view bossNameOf(s32 kind);
/** The stage mesh hidden by an SFXX arena cue, or empty when the boss has none.
 * This is the named world's object, not a mesh in the boss's animation tree. */
std::string_view bossArenaObject(s32 kind);
} // namespace gdl::game
