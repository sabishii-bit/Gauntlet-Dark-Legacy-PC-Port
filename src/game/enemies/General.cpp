#include "game/enemies/General.h"
namespace gdl::game {
CombatantDefinition General::definition() {
    CombatantDefinition out;
    out.name = "GENERAL";
    out.kind = 8;
    out.realmCostume = true;

    return out;
}
} // namespace gdl::game
