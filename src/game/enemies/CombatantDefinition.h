#pragma once
#include <string>

#include "engine/core/Types.h"

#include "game/enemies/CombatantKind.h"

namespace gdl::game {
/** Family policy supplied to shared combat, checked against the loaded descriptor family. */
struct CombatantDefinition {
    enum class Selection : u8 { Priority, Patterns };
    std::string name;
    CombatantKind kind = CombatantKind::Unknown;
    bool realmCostume = false;
    Selection selection = Selection::Priority;
    bool boundsToHome = false;
    f32 knockbackReduction = 0;
    bool breaksItems = false; ///< walks through chests and breaks barrels (fn_8005D5C8)
    std::string dropForm;
};
} // namespace gdl::game
