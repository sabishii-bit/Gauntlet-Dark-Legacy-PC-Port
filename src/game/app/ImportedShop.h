#pragma once

#include <span>

#include "game/characters/Party.h"

namespace gdl::game {
class PlayScene;

/** Commits an offline imported map's shop without reopening its scene. Refuses
 * a replaced/changed roster before modifying any participant. */
bool commitImportedShop(PlayScene& scene, std::span<const PartyMember> before,
                        std::span<const PartyMember> after);
} // namespace gdl::game
