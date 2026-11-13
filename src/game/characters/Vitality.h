#pragma once

#include "engine/core/Types.h"

namespace gdl::game {
struct CharacterSave;
struct ClassProgress;

/** The most health a character ever holds: the status box puts anything over it back each
 * frame (write_health_and_items, 0x8007516C and 0x8007521C; 9999.0f at 0x80347788). */
inline constexpr s32 kHealthLimit = 9999;

/** The most health a character of `level` has, which a heal tops it up to: 500 and 100 for
 * each level past the first, never past the limit (player_max_health, 0x8007857C). */
s32 mostHealth(s32 level);

/** Whether a heal would give the character nothing: its health is at its level's most. */
bool healthFull(const CharacterSave& save);

/** Up to `amount` more health, no further than the character's most and none once it is there
 * (heal_player, 0x800784E0). Returns what was given. */
s32 heal(CharacterSave& save, s32 amount);

/** `amount` more health past the level's most: a level gained (AddExp, 0x80076400) or a blow
 * through the halo taking a point back from Death (damage_enemy, 0x8004E8FC). The play scene
 * puts it back to its limit after each update, as the status box does. */
void addHealth(CharacterSave& save, s32 amount);

/** Puts stored health over `limit` back to it, as a save or a scenario is read. */
void limitHealth(ClassProgress& progress, s32 limit = kHealthLimit);

} // namespace gdl::game
