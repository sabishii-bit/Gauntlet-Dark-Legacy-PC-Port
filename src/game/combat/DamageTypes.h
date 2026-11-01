#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game::damage {

/**
 * The damage word (the original's DMG_ values): one bit space shared by a weapon powerup's
 * flags, a strike's damage type, a hit's flags and what a struck player suffers. The low
 * nibble is an element; the rest say what a blow does to what it hits or how it flies.
 */
inline constexpr u32 kElementMask = 0xF;
inline constexpr u32 kFire = 1;
inline constexpr u32 kElectric = 2;
inline constexpr u32 kLight = 3;
inline constexpr u32 kAcid = 4;
inline constexpr u32 kElementCount = 5;    ///< none, then the four
inline constexpr u32 kKnockBack = 0x10;    ///< shoves the struck body back
inline constexpr u32 kKnockDown = 0x20;    ///< puts it on the ground
inline constexpr u32 kBlownAway = 0x40;    ///< throws it away
inline constexpr u32 kStun = 0x80;         ///< holds it still a moment
inline constexpr u32 kKnockOver = 0x100;   ///< topples it
inline constexpr u32 kMagic = 0x200;       ///< potion magic: past armour, met by magic resistance
inline constexpr u32 kExplode = 0x400;     ///< bursts where it lands
inline constexpr u32 kGas = 0x800;         ///< poison gas: past armour, met by gas immunity
inline constexpr u32 kDeathStun = 0x1000;  ///< Death's touch
inline constexpr u32 kSpike = 0x2000;      ///< a spike's
inline constexpr u32 kGrabbed = 0x4000;    ///< the struck body is held
inline constexpr u32 kThrown = 0x8000;     ///< and thrown
inline constexpr u32 kWhirlwind = 0x10000; ///< spun away
inline constexpr u32 kArrow = 0x20000;     ///< flies as an arrow
inline constexpr u32 kFireBall = 0x40000;  ///< flies as a fireball
inline constexpr u32 kThreeWayShot = 0x80000;  ///< three missiles a throw
inline constexpr u32 kSuperShot = 0x100000;    ///< the charged super shot
inline constexpr u32 kReflect = 0x200000;      ///< the missile turns back off what it hits
inline constexpr u32 kFiveWayShot = 0x400000;  ///< five missiles a throw
inline constexpr u32 kHeal = 0x800000;         ///< what it takes feeds the caster's healing
inline constexpr u32 kNoHitEffect = 0x1000000; ///< leaves no mark where it lands
inline constexpr u32 kTurbo = 0x2000000;       ///< a turbo attack's
inline constexpr u32 kSticky = 0x4000000;      ///< the missile sticks where it lands
inline constexpr u32 kBossHit = 0x8000000;     ///< a boss's own blow
inline constexpr u32 kHammer = 0x10000000;     ///< the thunder hammer
inline constexpr u32 kRapidFire = 0x20000000;  ///< throws faster
inline constexpr u32 kLow = 0x40000000;        ///< flies low: passes a levitating body
/** The knocks a blow may carry, which knockback armour takes off it. */
inline constexpr u32 kKnocks = kKnockBack | kKnockDown | kBlownAway | kKnockOver;

/** The element a hit carries; nought for none. */
u32 element(u32 flags);

/** Whether the harm feeds the caster's healing (a caster's magic from level 25, the healing
 * weapon): only such a hit reaches do_heal_players. */
bool heals(u32 flags);

/** The level from which a caster's magic carries DMG_HEAL (start_magic, player.c 2066): the
 * flag the magic of a caster of `level` goes off with, or none. */
inline constexpr s32 kMagicHealsFrom = 25;
u32 magicHeal(s32 casterLevel);

/** Whether a hit leaves a mark where it lands (DMG_NOHITFX withholds it). */
bool marks(u32 flags);

/** How large the mark is drawn against the body it lands on: half its reach. */
inline constexpr f32 kHitEffectScale = 0.5f;

/** The mark most things show a hit of `element` (fn_800945D0's common tables): blood or the
 * element's own burst, and the death's; empty past the four elements. */
std::string_view hitEffect(u32 element, bool killed);

/** The colour code the original names an element's weapon effects by (DmgTypeDesc:
 * `WEAP_HOLD_<code>`, `WEAP_TW_<initial>`): fire's RED, lightning's BLU, light's YEL, acid's
 * GRE; empty for no element. */
std::string_view colourOf(u32 element);

/** The element that is a player colour's own (DamageColor's inverse): yellow's light, blue's
 * lightning, red's fire, green's acid; nought for a colour that is none's. */
u32 elementOfColour(s32 color);

/** A potion of the caster's own colour goes off a tenth stronger, in power and in harm
 * (start_magic, player.c 2062): what to scale a potion of `element` by for a caster of
 * `color`. */
inline constexpr f32 kOwnColourBonus = 1.1f;
f32 colourBonus(s32 color, u32 element);

} // namespace gdl::game::damage
