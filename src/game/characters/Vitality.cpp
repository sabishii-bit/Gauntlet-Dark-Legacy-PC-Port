#include "game/characters/Vitality.h"

#include <algorithm>

#include "engine/core/Types.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/Progression.h"

namespace gdl::game {

namespace {

constexpr s32 kHealthPerLevel = 100; ///< player_max_health's 100.0 at 0x803477D0

} // namespace

s32 mostHealth(s32 level) {
    // Its 500.0 (0x80347830) is a first-level character's whole health.
    return std::min(kHealthPerLevel * (level - 1) + kStartingHealth, kHealthLimit);
}

bool healthFull(const CharacterSave& save) {
    return save.health() >= mostHealth(experienceLevel(save.experience()));
}

s32 heal(CharacterSave& save, s32 amount) {
    const s32 most = mostHealth(experienceLevel(save.experience()));
    const s32 health = save.health();
    if (amount <= 0 || health >= most) {
        return 0;
    }
    const s32 given = std::min(amount, most - health);
    save.progress().health = health + given;
    return given;
}

void addHealth(CharacterSave& save, s32 amount) {
    save.progress().health = save.health() + amount;
}

void limitHealth(ClassProgress& progress, s32 limit) {
    progress.health = std::min(progress.health, limit);
}

} // namespace gdl::game
