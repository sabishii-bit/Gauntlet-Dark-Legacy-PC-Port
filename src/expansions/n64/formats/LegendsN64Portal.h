#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"

namespace gdl::formats {

/** PTRAP exit, not the paired PTEL teleport pads. Positions are authored feet
 * coordinates; the scene adapter applies the same native floor projection as
 * its corresponding visible fixture. Destination names come from game.bin's
 * realm/level tables, never from an assumed sequential map roster. */
struct LegendsN64Portal {
    u32 nativeIndex = 0;
    u32 destination = 0;
    std::string map;
    Vec3 position{0};
    f32 yaw = 0;
    f32 scale = 1;
    f32 radius = 0;
    f32 height = 0;
    u8 minimumPlayers = 1;
    u8 placementFlags = 0;
    std::string idle;
    std::string active;
    std::string starting;
    std::string closing;
};

/** Unmapped/dynamic destinations (including zero) return no invented map. */
std::optional<std::string> legendsN64Destination(std::span<const u8> program, u32 destination);

/** e006be0c's departure policy: ordinary stages return to Tower; only Tower
 * departures, secret entries and the authored Hell/Boss chains retain targets.
 * A source map absent from the native tables is not assigned a guessed route. */
std::optional<std::string> legendsN64ExitDestination(std::span<const u8> program, u32 destination,
                                                     std::string_view sourceMap);

/** Native subtype5 level exits only. Subtype6 secret-level returns need their
 * separate campaign-return context and are deliberately not admitted here. */
std::vector<LegendsN64Portal> decodeLegendsN64Portals(std::span<const u8> placements,
                                                      std::span<const u8> templates,
                                                      std::span<const u8> program,
                                                      std::string_view sourceMap = {});

} // namespace gdl::formats
