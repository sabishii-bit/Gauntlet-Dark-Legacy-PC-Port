#pragma once

#include <cstdint>
#include <string_view>

#include "game/players/Inventory.h"

namespace gdl::game {

/** The flags the original gives the powerups this far along. */
namespace powerup {
inline constexpr std::int32_t kWeapon = 5;
inline constexpr std::int32_t kArmor = 6;
inline constexpr std::int32_t kSpeed = 7;
inline constexpr std::int32_t kMagic = 8;
inline constexpr std::int32_t kSpecial = 9;
inline constexpr std::uint32_t kThreeWayShot = 0x00080000; ///< weapon
inline constexpr std::uint32_t kFiveWayShot = 0x00400000;  ///< weapon
inline constexpr std::uint32_t kInvisible = 0x00000004;    ///< special
inline constexpr std::uint32_t kGrowth = 0x00000100;       ///< special
} // namespace powerup

/**
 * What the powerups a character wears add up to, gathered the way the original gathers them
 * each frame from the slots that are held and switched on: the weapon and armour flags, the
 * special flags, and what speed and magic powerups add.
 */
struct PowerupEffects {
    static constexpr float kGrowthScale = 1.3f;
    static constexpr float kInvisibleAlpha = 95.0f / 255.0f; ///< how solid an unseen body shows
    static constexpr float kInvisibleWaver = 16.0f / 255.0f; ///< and how much that wavers

    std::uint32_t weapon = 0;
    std::uint32_t armor = 0;
    std::uint32_t special = 0;
    float paceAdd = 0.0f; ///< units a second onto the character's pace
    float magicAdd = 0.0f;

    static PowerupEffects of(const Inventory& inventory);

    static constexpr float kLeastMagicPower = 8.0f; ///< with no magic at all
    static constexpr float kMostMagicPower = 32.0f; ///< at a magic stat of 1000
    /** The power a character's potions go off with: by their magic stat, and what magic
     * powerups add. */
    float magicPower(std::int32_t magicStat) const;

    /** How many weapons a throw lets fly: one, or three or five spread fifteen degrees apart. */
    std::int32_t shots() const;
    bool invisible() const { return (special & powerup::kInvisible) != 0; }
    bool grown() const { return (special & powerup::kGrowth) != 0; }
    /** How solid the body is drawn `seconds` into the level. */
    float bodyAlpha(float seconds) const;
};

/** The text id of a powerup's name for the selector, by its kind and flags: the first of the
 * original's list whose flags it carries, else its kind's general name. */
std::string_view powerupTextId(std::int32_t kind, std::uint32_t flags);

} // namespace gdl::game
