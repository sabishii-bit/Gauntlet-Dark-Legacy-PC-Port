#pragma once
#include <string_view>

#include "engine/core/Types.h"

#include "game/enemies/CombatantDefinition.h"
namespace gdl::game {
/** Boss identity and combat policy; encounter progression belongs to Bosses. */
CombatantDefinition bossDefinition(std::string_view name);
std::string_view bossNameOf(s32 kind);
} // namespace gdl::game
