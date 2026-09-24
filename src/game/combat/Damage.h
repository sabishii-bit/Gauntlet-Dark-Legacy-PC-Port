#pragma once

#include "engine/core/Types.h"

namespace gdl::game {
/** A hit after armour, elemental affinities and protective items have been applied. */
struct Damage {
    f32 amount = 0;
    u32 flags = 0;

    static constexpr u32 kElement = 0xF;
    static constexpr u32 kMagic = 0x200;
    static constexpr u32 kGas = 0x800;
    static constexpr u32 kLow = 0x40000000;
    static constexpr u32 kInvulnerable = 0x10000;
    static constexpr u32 kGoldInvulnerable = 0x100000;

    /** Negative results heal. Boss encounters use their own elemental multipliers. */
    static Damage modify(f32 amount, u32 flags, u32 shield, f32 armor, bool bossEncounter);
};
} // namespace gdl::game
