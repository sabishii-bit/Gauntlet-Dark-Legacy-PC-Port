#pragma once

#include <span>
#include <string_view>

#include "engine/core/Types.h"

#include "game/players/Relics.h"

namespace gdl::game {

/** Follow-up speeches and route reveals after the party installs its tower relics. */
struct TowerCompletion {
    enum class Kind : u8 { MoreShards, Window, TwelveWaiting, Underworld, Garm };
    static constexpr u16 kKnown = Relics::kTowerCeremonyMask;
    static constexpr s32 kRevealTicks = 180;
    static constexpr u16 bit(Kind kind) { return static_cast<u16>(1U << static_cast<u8>(kind)); }
    static u16 pending(std::span<const Relics> party);
    static std::string_view message(Kind kind);
    static std::string_view voice(Kind kind);
    static u32 camera(Kind kind);
    static std::string_view portal(Kind kind);
    static bool reveals(Kind kind) { return !portal(kind).empty(); }
};

} // namespace gdl::game
