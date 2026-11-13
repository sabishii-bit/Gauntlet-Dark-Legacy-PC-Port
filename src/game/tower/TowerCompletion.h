#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "engine/core/Types.h"

#include "game/characters/Relics.h"
#include "game/tower/LevelBeaten.h"

namespace gdl::game {

/** Follow-up speeches and route reveals after the party installs its tower relics, and
 * Sumner's reminder to a party back from the battlefield without the thirteenth runestone. */
struct TowerCompletion {
    /** What follows. The reminder (ThirteenMissing) answers the level just beaten, so it is
     * never kept for a later visit: its bit lies outside kKnown. */
    enum class Kind : u8 { MoreShards, Window, TwelveWaiting, Underworld, Garm, ThirteenMissing };
    static constexpr u16 kKnown = Relics::kTowerCeremonyMask;
    static constexpr s32 kRevealTicks = 180;
    static constexpr u16 bit(Kind kind) { return static_cast<u16>(1U << static_cast<u8>(kind)); }
    static u16 pending(std::span<const Relics> party);
    /** Whether Sumner tells a party back from `beaten` that it lacks the thirteenth runestone:
     * the battlefield's third level beaten, no runestone new to the tower and the thirteenth
     * set in its place by nobody (TowerCheckMessages 0x800A2F60-0x800A3018). */
    static bool remindsThirteenth(std::span<const Relics> party,
                                  const std::optional<LevelBeaten>& beaten);
    static std::string_view message(Kind kind);
    static std::string_view voice(Kind kind);
    static u32 camera(Kind kind);
    static std::string_view portal(Kind kind);
    static bool reveals(Kind kind) { return !portal(kind).empty(); }
};

} // namespace gdl::game
