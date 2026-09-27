#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/**
 * The colours and sizes of the lights the original's effects and players give off
 * (sfx.c's light_color, pclr_idx and mclr_idx, player.c's lantern and damage-row tables).
 */
struct DynamicLights {
    static constexpr f32 kEffectIntensity = 2.0f;     ///< sfx_lightattn
    static constexpr f32 kEffectLift = 1.0f;          ///< an effect's light hangs this over it
    static constexpr f32 kChargeRadius = 10.0f;       ///< StartComboFX
    static constexpr f32 kMagicRadiusPerPower = 1.5f; ///< StartMagicFX, StartShieldFX
    static constexpr f32 kBlastRadiusScale = 2.0f;    ///< StartExplosion: twice the blast's
    static constexpr f32 kLanternRadius = 20.0f;      ///< player_lightrad, in a dark level
    static constexpr f32 kLanternIntensity = 10.0f;   ///< player_lightattn
    static constexpr f32 kLanternLift = 10.0f;        ///< player_lightdy
    static constexpr u32 kDarkLevel = 8;              ///< the level flag that puts the lights out

    /** A costume colour's light (yellow, blue, red, green: pclr_idx over light_color). */
    static Vec3 ofCostume(s32 color);
    /** A damage type's (normal white, fire red, electric white, light yellow, acid green). */
    static Vec3 ofDamage(s32 type);
    /** What a potion of `kind` gives off: its element's. */
    static Vec3 ofPotion(s32 kind);
    /** A costume colour's lantern in a dark level. */
    static Vec3 lantern(s32 color);
    /** A character class's own (plight_color: warrior red, valkyrie blue, wizard cyan,
     * archer green, and again for the four after). */
    static Vec3 ofClass(s32 character);
    /** An explosion's: red. */
    static Vec3 blast() { return Vec3{2.0f, 0.0f, 0.0f}; }
};

} // namespace gdl::game
