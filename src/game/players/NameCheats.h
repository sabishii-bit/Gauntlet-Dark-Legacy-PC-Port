#pragma once

#include <span>
#include <string_view>

#include "engine/core/Types.h"

#include "game/players/CharacterSave.h"

namespace gdl::game {

/** A name-selected costume, using its ordinary class's animations and combat data. */
struct HiddenCostume {
    std::string_view name;
    s32 character;
    s32 color;
    std::string_view directory;
};

/** The enabled name-selected costumes. Developer-only costumes are excluded. */
std::span<const HiddenCostume> hiddenCostumes();
const HiddenCostume* hiddenCostume(std::string_view name);

/** Apply the six-character name codes when a character is selected or enters a level.
 * Codes are case-sensitive; powerups use ordinary inventory slots and can be toggled.
 * Returns whether a code was recognized, without changing an ordinary character. */
bool applyNameCheats(CharacterSave& save);

} // namespace gdl::game
