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
inline constexpr u32 kThreeWayShot = 0x00080000; ///< weapon
inline constexpr u32 kFiveWayShot = 0x00400000;  ///< weapon
inline constexpr u32 kInvisible = 0x00000004;    ///< special
inline constexpr u32 kGrowth = 0x00000100;       ///< special
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
    bool grown() const { return (special & powerup::kGrowth) != 0; }
    /** How solid the body is drawn `seconds` into the level. */
    f32 bodyAlpha(f32 seconds) const;
};

/** The text id of a powerup's name for the selector, by its kind and flags: the first of the
 * original's list whose flags it carries, else its kind's general name. */
std::string_view powerupTextId(s32 kind, u32 flags);

} // namespace gdl::game
