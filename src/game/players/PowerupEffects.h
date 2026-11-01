#pragma once

#include <string_view>

#include "engine/core/Types.h"

#include "game/characters/Inventory.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"

namespace gdl::game {

/**
 * The powerup kinds (the item subtype a slot came from) and the special word (the original's
 * SPECIAL_TYPE), the flags a special powerup carries. A weapon powerup's flags are the damage
 * word (damage::) and an armour powerup's the armour word (armor::).
 */
namespace powerup {
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
} // namespace powerup

/**
 * What the powerups a character wears add up to, gathered the way the original gathers them
 * each frame from the slots that are held and switched on: the weapon and armour flags, the
 * special flags, and what speed and magic powerups add.
 */
struct PowerupEffects {
    static constexpr f32 kGrowthScale = 1.3f;
    static constexpr f32 kLevitationLift = 1.5f;           ///< how high a levitating body is held
    static constexpr f32 kInvisibleAlpha = 95.0f / 255.0f; ///< how solid an unseen body shows
    static constexpr f32 kInvisibleWaver = 16.0f / 255.0f; ///< and how much that wavers
    static constexpr f32 kWarningSeconds = 3.0f; ///< the last of a powerup, shown by blinking
    static constexpr f32 kBlinkRate = 8.0f;      ///< eighths of a second, on and off

    /** The chrome skin invulnerability shows (PlayerProcessPowerups). */
    enum class Chrome : u8 { None, Silver, Gold };

    u32 weapon = 0;
    u32 armor = 0;
    u32 special = 0;
    f32 paceAdd = 0.0f; ///< units a second onto the character's pace
    f32 magicAdd = 0.0f;
    /** The invisibility and invulnerability left, by the original's rule over the slots
     * (alpha_time, weapon_time): the longest, or one for good (under none) met before it. */
    f32 invisibleLeft = 0.0f;
    f32 invulnerableLeft = 0.0f;

    static PowerupEffects of(const Inventory& inventory);

    static constexpr f32 kLeastMagicPower = 8.0f; ///< with no magic at all
    static constexpr f32 kMostMagicPower = 32.0f; ///< at a magic stat of 1000
    /** The power a character's potions go off with: by their magic stat, and what magic
     * powerups add. */
    f32 magicPower(s32 magicStat) const;

    /** How many weapons a throw lets fly: one, or three or five spread fifteen degrees apart. */
    s32 shots() const;
    bool invisible() const { return (special & powerup::kInvisible) != 0; }
    bool invulnerable() const { return (armor & armor::kInvulnerable) != 0; }
    bool xray() const { return (special & powerup::kXRay) != 0; }
    bool grown() const { return (special & powerup::kGrowth) != 0; }
    bool levitating() const { return (special & powerup::kLevitation) != 0; }
    /** How far over the floor the body is held: the wings' lift (PlayerMotion, pmotion.c
     * 2908), else nothing. */
    f32 lift() const { return levitating() ? kLevitationLift : 0.0f; }
    bool preventsKnockback() const { return (armor & armor::kStandsFirm) != 0; }
    /** How solid the body is drawn: unseen, wavering with the time left, and shown whole
     * every other eighth of a second in the last three. */
    f32 bodyAlpha() const;
    /** The chrome skin shown, none while it blinks off in its last three seconds. */
    Chrome chrome() const;
    /** Whether a powerup with `left` seconds is blinked off this moment. */
    static bool blinkedOff(f32 left);
};

/** The text id of a powerup's name for the selector, by its kind and flags: the first of the
 * original's list whose flags it carries, an amulet's element, else its kind's general name. */
std::string_view powerupTextId(s32 kind, u32 flags);

/** Coin stages cannot use Stop Time. Keep the carried item for the return journey. */
bool powerupAllowedInChallenge(s32 kind, u32 flags);

} // namespace gdl::game
