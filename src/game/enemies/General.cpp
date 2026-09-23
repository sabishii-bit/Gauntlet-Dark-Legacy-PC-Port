#include "game/enemies/General.h"
namespace gdl::game {
CombatantDefinition General::definition() {
    CombatantDefinition out;
    out.name = "GENERAL";
    out.kind = CombatantKind::General;
    out.realmCostume = true;

    return out;
}
} // namespace gdl::game
