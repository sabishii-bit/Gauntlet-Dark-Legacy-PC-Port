#include "game/enemies/BossDefinition.h"

#include "engine/core/Strings.h"
namespace gdl::game {
std::string_view bossNameOf(s32 kind) {
    switch (kind) {
    case 34: return "DRAGON";
    case 35: return "CHIMERA";
    case 36: return "DJINN";
    case 37: return "DRIDER";
    case 38: return "PBOSS";
    case 39: return "YETI";
    case 40: return "WRAITH";
    case 41: return "LICH";
    case 42: return "SKORNE1";
    case 43: return "SKORNE2";
    case 44: return "GARM";
    default: return "";
    }
}

CombatantDefinition bossDefinition(std::string_view name) {
    CombatantDefinition out;
    out.name = normalizeAssetName(name);
    out.kind = CombatantKind::Boss;
    out.selection = CombatantDefinition::Selection::Patterns;
    out.boundsToHome = true;
    return out;
}
} // namespace gdl::game
