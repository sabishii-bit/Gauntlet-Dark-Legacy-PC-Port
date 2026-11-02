#pragma once

#include "engine/core/Types.h"

namespace gdl::game::powerup {

/**
 * The powerup kinds (the item subtype a slot came from) and the special word (the original's
 * SPECIAL_TYPE), the flags a special powerup carries. A weapon powerup's flags are the damage
 * word (damage::) and an armour powerup's the armour word (armor::).
 */
inline constexpr s32 kWeapon = 5;
inline constexpr s32 kArmor = 6;
inline constexpr s32 kSpeed = 7;
inline constexpr s32 kMagic = 8;
inline constexpr s32 kSpecial = 9;
inline constexpr u32 kLevitation = 0x1;
inline constexpr u32 kXRay = 0x2; ///< see inside the nearest closed chest
inline constexpr u32 kInvisible = 0x4;
inline constexpr u32 kStopTime = 0x8; ///< halt ordinary opponents and hazards
inline constexpr u32 kFireBreath = 0x10;
inline constexpr u32 kAcidBreath = 0x20;
inline constexpr u32 kLightningBreath = 0x40;
inline constexpr u32 kBreath = kFireBreath | kAcidBreath | kLightningBreath;
inline constexpr u32 kPhoenix = 0x80; ///< temporary fire-spitting companion
inline constexpr u32 kGrowth = 0x100;
inline constexpr u32 kEnemyShrink = 0x200; ///< the swarm at two thirds a wearer
inline constexpr u32 kPojo = 0x400;        ///< Pojo speaks for the character
inline constexpr u32 kSkorneHorns = 0x1000;
inline constexpr u32 kSkorneMask = 0x2000;
inline constexpr u32 kRightGauntlet = 0x4000;
inline constexpr u32 kLeftGauntlet = 0x8000;
inline constexpr u32 kSpeedBoost = 0x10000;
inline constexpr u32 kHealth = 0x20000;       ///< SPECIAL_HEALTH
inline constexpr u32 kDummy = 0x40000;        ///< SPECIAL_DUMMY
inline constexpr u32 kTurbo = 0x80000;        ///< immediate meter refill
inline constexpr u32 kMikey = 0x100000;       ///< drop or dismiss the swarm decoy
inline constexpr u32 kHandOfDeath = 0x200000; ///< return enemy melee damage
inline constexpr u32 kHealthVamp = 0x400000;  ///< return melee as magic and heal

} // namespace gdl::game::powerup
