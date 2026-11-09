#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"

#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/shared/level/LegendsContainers.h"

namespace gdl::game {

/** A native container placement: the shared container definition and the
 * PPower placed inside it, which only the N64 scene binds. */
struct LegendsN64Container {
    LegendsContainerDefinition definition;
    std::optional<formats::LegendsN64Fixture> contents;
};

/** Native PCon/Ppower records, not fabricated GameCube item definitions.
 * Unknown scripted contents remain explicit diagnostics, never guessed loot. */
std::optional<LegendsN64Container>
decodeLegendsN64Container(const formats::LegendsN64Fixture& fixture, std::span<const u8> containers,
                          std::span<const u8> powerups);

/** Red and green explosive barrels are PTrap records, not PCon containers. They
 * hold nothing. */
std::optional<LegendsN64Container>
decodeLegendsN64Explosive(const formats::LegendsN64Fixture& fixture, std::span<const u8> traps);

} // namespace gdl::game
