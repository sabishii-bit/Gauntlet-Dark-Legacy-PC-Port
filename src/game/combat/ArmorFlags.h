#pragma once

#include "engine/core/Types.h"

namespace gdl::game::armor {

/**
 * The armour word (the original's ARMOR_TYPE): the flags an armour powerup carries and what a
 * character wears adds up to. A resistance halves an element's harm and an immunity takes it
 * all; the rest guard against particular blows or strike back at what the wearer touches.
 */
inline constexpr u32 kResistFire = 0x1;
inline constexpr u32 kResistElectric = 0x2;
inline constexpr u32 kResistLight = 0x4;
inline constexpr u32 kResistAcid = 0x8; ///< the gas mask
inline constexpr u32 kResistMagic = 0x10;
inline constexpr u32 kImmuneShift = 8; ///< an element's immunity bit is its resistance bit shifted
inline constexpr u32 kImmuneFire = 0x100;
inline constexpr u32 kImmuneElectric = 0x200;
inline constexpr u32 kImmuneLight = 0x400;
inline constexpr u32 kImmuneAcid = 0x800;
inline constexpr u32 kImmuneMagic = 0x1000;
inline constexpr u32 kImmuneGas = 0x2000;          ///< also the gas helmet the figure wears
inline constexpr u32 kInvulnerable = 0x10000;      ///< silver: no harm at all
inline constexpr u32 kReflect = 0x20000;           ///< turns missiles back; borne on the arm
inline constexpr u32 kNoKnockback = 0x40000;       ///< a blow's knocks are taken off it
inline constexpr u32 kAntiDeath = 0x80000;         ///< the halo: Death takes nothing
inline constexpr u32 kGoldInvulnerable = 0x100000; ///< gold: harm heals a tenth of itself
inline constexpr u32 kFireArmor = 0x200000;        ///< burns what its bearer is against
inline constexpr u32 kElectricArmor = 0x400000;    ///< shocks what its bearer is against
inline constexpr u32 kProtect = 0x800000;          ///< ARMOR_PROTECT
inline constexpr u32 kReflect2 = 0x1000000;        ///< ARMOR_REFLECT2
/** The three borne on the arm, which the figure shows as a shield. */
inline constexpr u32 kArmShields = kReflect | kFireArmor | kElectricArmor;
/** What keeps a blow from knocking its wearer about. */
inline constexpr u32 kStandsFirm = kInvulnerable | kNoKnockback | kGoldInvulnerable;
/** The two armours that turn a missile back off their wearer. */
inline constexpr u32 kReflects = kReflect | kReflect2;

/** The resistance bit of `element` (1 to 4), and its immunity bit. */
constexpr u32 resist(u32 element) {
    return 1U << (element - 1);
}
constexpr u32 immune(u32 element) {
    return resist(element) << kImmuneShift;
}

} // namespace gdl::game::armor
