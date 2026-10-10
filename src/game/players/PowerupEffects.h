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
inline constexpr u32 kSpeedBoost = 0x00010000;   ///< special
inline constexpr u32 kInvisible = 0x00000004;    ///< special
inline constexpr u32 kXRay = 0x00000002;         ///< special: see inside the nearest closed chest
inline constexpr u32 kStopTime = 0x00000008;     ///< special: halt ordinary opponents and hazards
inline constexpr u32 kGrowth = 0x00000100;       ///< special
inline constexpr u32 kEnemyShrink = 0x00000200;  ///< special: the swarm at two thirds a wearer
inline constexpr u32 kLevitation = 1;            ///< special
inline constexpr u32 kTurbo = 0x00080000;        ///< special, immediate meter refill
inline constexpr u32 kMikey = 0x00100000;        ///< special: drop or dismiss the swarm decoy
inline constexpr u32 kHandOfDeath = 0x00200000;  ///< special: return enemy melee damage
inline constexpr u32 kHealthVamp = 0x00400000;   ///< special: return melee as magic and heal
inline constexpr u32 kPojo = 0x00000400;         ///< special: Pojo speaks for the character
inline constexpr u32 kInvulnerable = 0x00010000; ///< armor
inline constexpr u32 kGoldInvulnerable = 0x00100000; ///< armor
inline constexpr u32 kReflectShield = 0x00020000; ///< armor: turns missiles back, borne on the arm
inline constexpr u32 kFireShield = 0x00200000;    ///< armor: burns what its bearer is against
inline constexpr u32 kLightningShield = 0x00400000; ///< armor: shocks what its bearer is against
inline constexpr u32 kShields = kReflectShield | kFireShield | kLightningShield;
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
    bool xray() const { return (special & powerup::kXRay) != 0; }
    bool grown() const { return (special & powerup::kGrowth) != 0; }
    bool levitating() const { return (special & powerup::kLevitation) != 0; }
    /** How far over the floor the body is held: the wings' lift (PlayerMotion, pmotion.c
     * 2908), else nothing. */
    f32 lift() const { return levitating() ? kLevitationLift : 0.0f; }
    bool preventsKnockback() const { return (armor & 0x150000) != 0; }
    /** How solid the body is drawn: unseen, wavering with the time left, and shown whole
     * every other eighth of a second in the last three. */
    f32 bodyAlpha() const;
    /** The chrome skin shown, none while it blinks off in its last three seconds. */
    Chrome chrome() const;
    /** Whether a powerup with `left` seconds is blinked off this moment. */
    static bool blinkedOff(f32 left);
};

/** The text id of a powerup's name for the selector, by its kind and flags: the first of the
 * original's list whose flags it carries, else its kind's general name. */
std::string_view powerupTextId(s32 kind, u32 flags);

/** Coin stages cannot use Stop Time. Keep the carried item for the return journey. */
bool powerupAllowedInChallenge(s32 kind, u32 flags);

} // namespace gdl::game
