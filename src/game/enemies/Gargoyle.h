#pragma once
#include <string_view>

#include "game/enemies/CombatantDefinition.h"
namespace gdl::game {
/** Form-specific fighter whose defeated form identifies its collectible key. */
struct Gargoyle {
    /** Accepts the roster suffix (eagl/lion/serp) or an explicit GAR_ archive name. */
    static CombatantDefinition definition(std::string_view form = {});
};
} // namespace gdl::game
