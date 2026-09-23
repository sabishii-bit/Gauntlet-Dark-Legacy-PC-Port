#include "game/enemies/Golem.h"
namespace gdl::game {
CombatantDefinition Golem::definition() {
    CombatantDefinition out;
    out.name = "GOLEM";
    out.kind = CombatantKind::Golem;
    out.realmCostume = true;
    out.knockbackReduction = 5.0f;
    return out;
}
} // namespace gdl::game
