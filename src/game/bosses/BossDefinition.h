#pragma once
#include <string_view>

#include "engine/core/Types.h"

#include "game/critters/CombatantDefinition.h"
namespace gdl::game {
/** Boss identity and combat policy; encounter progression belongs to Bosses. */
CombatantDefinition bossDefinition(std::string_view name);
/** Generator art in the boss archive, used when the level has no BOSSGEN tree. */
struct BossGeneratorVisual {
    std::string_view tree = "BOSSGEN";
    bool settled = false; ///< retain the completed impact pose instead of replaying its landing
};
BossGeneratorVisual bossGeneratorVisual(s32 kind);
} // namespace gdl::game
