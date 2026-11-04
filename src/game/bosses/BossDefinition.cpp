#include "game/bosses/BossDefinition.h"

#include "engine/core/Strings.h"
namespace gdl::game {
CombatantDefinition bossDefinition(std::string_view name) {
    CombatantDefinition out;
    out.name = normalizeAssetName(name);
    out.kind = CombatantKind::Boss;
    out.selection = CombatantDefinition::Selection::Patterns;
    out.boundsToHome = true;
    out.projectileEndVisual = out.name == "YETI";
    out.targeting = CombatantDefinition::Targeting::AngerRanked;
    out.seesInvisible = true;
    out.harmScale = CombatantDefinition::HarmScale::PartySize;
    out.shrinks = false;
    out.shoved = false;
    out.departure = CombatantDefinition::Departure::Fades;
    return out;
}

BossGeneratorVisual bossGeneratorVisual(s32 kind) {
    // The egg's impact leaves its L1 mesh at full size in the final pose.
    // Lich has a separate generator tree whose ACTIVE sequence repeats.
    // Neither archive exposes these bodies under the generic BOSSGEN name.
    switch (kind) {
    case 37: return {"GENPROJHIT", true};
    case 41: return {"ATK12GEN", false};
    default: return {};
    }
}
} // namespace gdl::game
