#pragma once

#include "game/characters/CharacterSave.h"

namespace gdl::game {

/** Returns a local native-map visit to its parked character. Refuses if another
 * console action changed that character in the meantime; never overwrites it. */
bool commitLegendsVisit(CharacterSave& live, const CharacterSave& before,
                        const CharacterSave& after);

/** Preserve permanent native campaign awards when ordinary level earnings are
 * abandoned. Character identity must match; other classes/platforms are untouched. */
bool retainLegendsProgress(CharacterSave& restored, const CharacterSave& current,
                           LegendsPlatform platform);

} // namespace gdl::game
