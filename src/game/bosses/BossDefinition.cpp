#include "game/bosses/BossDefinition.h"

#include "engine/core/Strings.h"

#include "game/bosses/BossTraits.h"
#include "game/level/BossKinds.h"
namespace gdl::game {
CombatantDefinition bossDefinition(std::string_view name) {
    CombatantDefinition out;
    out.name = normalizeAssetName(name);
    out.kind = CombatantKind::Boss;
    out.selection = CombatantDefinition::Selection::Patterns;
    out.boundsToHome = true;
    const BossStage* stage = bossStageNamed(out.name);
    const BossTraits* traits = stage != nullptr ? bossTraitsOf(stage->kind) : nullptr;
    out.projectileEndVisual = traits != nullptr && traits->projectileEndVisual;
    out.targeting = CombatantDefinition::Targeting::AngerRanked;
    out.seesInvisible = true;
    out.harmScale = CombatantDefinition::HarmScale::PartySize;
    out.shrinks = false;
    out.shoved = false;
    out.departure = CombatantDefinition::Departure::Fades;
    return out;
}

BossGeneratorVisual bossGeneratorVisual(s32 kind) {
    const BossTraits* traits = bossTraitsOf(kind);
    return traits != nullptr ? traits->generator : BossGeneratorVisual{};
}
} // namespace gdl::game
