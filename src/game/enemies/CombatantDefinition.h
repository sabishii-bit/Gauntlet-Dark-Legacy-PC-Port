#pragma once
#include <string>

#include "engine/core/Types.h"

namespace gdl::game {
/** Family policy supplied to the shared combat runtime, independent of file-format ids. */
struct CombatantDefinition {
    enum class Selection : u8 { Priority, Patterns };
    std::string name;
    s32 kind = 0;
    bool realmCostume = false;
    Selection selection = Selection::Priority;
    bool boundsToHome = false;
    f32 knockbackReduction = 0;
    std::string dropForm;
};
} // namespace gdl::game
