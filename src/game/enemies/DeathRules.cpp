#include "game/enemies/DeathRules.h"

#include <algorithm>

namespace gdl::game {
DeathForm DeathRules::form(s32 tier) {
    return tier == 2 ? DeathForm::Black : DeathForm::Red;
}
std::string_view DeathRules::effect(DeathForm form) {
    return form == DeathForm::Black ? "DEATH_EXP" : "DEATH_ARC";
}
s32 DeathRules::experience(s32 level, bool stealing) {
    if (stealing && level >= 99) {
        return 0;
    }
    const s32 next = level > 60 ? 4600 : (std::max(level, 1) - 1) * 60 + 1000;
    return next / 100;
}
f32 DeathRules::magicHealing(s32 level, f32 remainingHealth) {
    return level > 75 ? (0.032f * static_cast<f32>(level - 75) + 0.2f) * remainingHealth : 0;
}
} // namespace gdl::game
