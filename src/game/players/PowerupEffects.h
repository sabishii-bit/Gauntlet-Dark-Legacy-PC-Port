#pragma once

#include <string_view>

#include "engine/core/Types.h"

#include "game/players/Inventory.h"

namespace gdl::game {

/** The flags the original gives the powerups this far along. */
namespace powerup {
inline constexpr s32 kWeapon = 5;
inline constexpr s32 kArmor = 6;
inline constexpr s32 kSpeed = 7;
inline constexpr s32 kMagic = 8;
inline constexpr s32 kSpecial = 9;
inline constexpr u32 kThreeWayShot = 0x00080000;  ///< weapon
inline constexpr u32 kFiveWayShot = 0x00400000;   ///< weapon
inline constexpr u32 kSuperShot = 0x00100000;     ///< weapon
inline constexpr u32 kReflect = 0x00200000;       ///< weapon
inline constexpr u32 kRapidFire = 0x20000000;     ///< weapon
inline constexpr u32 kThunderHammer = 0x10000000; ///< weapon
inline constexpr u32 kFireBreath = 0x10;          ///< special
inline constexpr u32 kAcidBreath = 0x20;
inline constexpr u32 kLightningBreath = 0x40;
inline constexpr u32 kBreath = kFireBreath | kAcidBreath | kLightningBreath;
inline constexpr u32 kPhoenix = 0x80; ///< special: temporary fire-spitting companion
inline constexpr u32 kSkorneHorns = 0x1000;
inline constexpr u32 kSkorneMask = 0x2000;
inline constexpr u32 kRightGauntlet = 0x4000;
inline constexpr u32 kLeftGauntlet = 0x8000;
inline constexpr u32 kSpeedBoost = 0x00010000;  ///< special
inline constexpr u32 kInvisible = 0x00000004;   ///< special
inline constexpr u32 kXRay = 0x00000002;        ///< special: see inside the nearest closed chest
inline constexpr u32 kGrowth = 0x00000100;      ///< special
inline constexpr u32 kLevitation = 1;           ///< special
inline constexpr u32 kTurbo = 0x00080000;       ///< special, immediate meter refill
inline constexpr u32 kHandOfDeath = 0x00200000; ///< special: return enemy melee damage
inline constexpr u32 kHealthVamp = 0x00400000;  ///< special: return melee as magic and heal
} // namespace powerup

/**
 * What the powerups a character wears add up to, gathered the way the original gathers them
 * each frame from the slots that are held and switched on: the weapon and armour flags, the
 * special flags, and what speed and magic powerups add.
 */
struct PowerupEffects {
    static constexpr f32 kGrowthScale = 1.3f;
    static constexpr f32 kInvisibleAlpha = 95.0f / 255.0f; ///< how solid an unseen body shows
    static constexpr f32 kInvisibleWaver = 16.0f / 255.0f; ///< and how much that wavers

    u32 weapon = 0;
    u32 armor = 0;
    u32 special = 0;
    f32 paceAdd = 0.0f; ///< units a second onto the character's pace
    f32 magicAdd = 0.0f;

    static PowerupEffects of(const Inventory& inventory);

    static constexpr f32 kLeastMagicPower = 8.0f; ///< with no magic at all
    static constexpr f32 kMostMagicPower = 32.0f; ///< at a magic stat of 1000
    /** The power a character's potions go off with: by their magic stat, and what magic
     * powerups add. */
    f32 magicPower(s32 magicStat) const;

    /** How many weapons a throw lets fly: one, or three or five spread fifteen degrees apart. */
    s32 shots() const;
    bool invisible() const { return (special & powerup::kInvisible) != 0; }
    bool xray() const { return (special & powerup::kXRay) != 0; }
    bool grown() const { return (special & powerup::kGrowth) != 0; }
    bool preventsKnockback() const { return (armor & 0x150000) != 0; }
    /** How solid the body is drawn `seconds` into the level. */
    f32 bodyAlpha(f32 seconds) const;
};

/** The text id of a powerup's name for the selector, by its kind and flags: the first of the
 * original's list whose flags it carries, else its kind's general name. */
std::string_view powerupTextId(s32 kind, u32 flags);

} // namespace gdl::game
