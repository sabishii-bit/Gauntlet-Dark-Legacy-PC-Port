#include "game/enemies/Golem.h"
namespace gdl::game {
CombatantDefinition Golem::definition(char realm) {
    CombatantDefinition out;
    out.name = "GOLEM";
    // init_next_level (0x8005638C) selects separate move/collision tables for
    // worlds 9 (Ice Domain) and 6 (Underworld). All three still name the golem
    // costume folder; a different model alone cannot supply these behaviours.
    if (realm == 'I' || realm == 'i') {
        out.name = "GOLEMI";
    } else if (realm == 'F' || realm == 'f') {
        out.name = "GOLEMF";
    }
    out.kind = CombatantKind::Golem;
    out.realmCostume = true;
    out.knockbackReduction = 5.0f;
    out.breaksItems = true;
    return out;
}
} // namespace gdl::game
