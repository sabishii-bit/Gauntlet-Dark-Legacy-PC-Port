#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game::damage {

/** The bits of a hit's damage type that name what kind of harm it is (the original's DMG_
 * values): an element in the low nibble, and the marks a blow leaves or withholds. */
inline constexpr u32 kElementMask = 0xF;
inline constexpr u32 kFire = 1;
inline constexpr u32 kElectric = 2;
inline constexpr u32 kLight = 3;
inline constexpr u32 kAcid = 4;
inline constexpr u32 kElementCount = 5;        ///< none, then the four
inline constexpr u32 kHeal = 0x800000;         ///< what it takes feeds the caster's healing
inline constexpr u32 kNoHitEffect = 0x1000000; ///< leaves no mark where it lands

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
