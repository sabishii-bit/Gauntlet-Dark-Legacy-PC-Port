#pragma once

#include <string_view>

#include "game/effects/EffectTrees.h"
#include "game/party/PlayerMissiles.h"

namespace gdl::game {
/** Callback-free preparation of an actual native potion release. The caller
 * retains inventory spending, the shared unspecified-colour cycle, sound,
 * damage/healing and trusted artwork bindings. Preparing a descriptor is not
 * permission to consume a potion or dispatch an impact. */
struct PotionEmission {
    static constexpr f32 kBurstPerPower = 0.03125f;
    static constexpr f32 kDamage = 40.0f;

    struct Look {
        std::string_view bottle;
        std::string_view burst;
        std::string_view sound;
    };
    struct Context {
        s32 owner = 0;
        Vec3 position{0};
        Vec3 facing{0, 0, 1};
        s32 resolvedKind = 0; ///< never advances the caller's shared kind-zero cycle
        f32 power = 0;        ///< includes the bonus of the stored, not resolved, colour
        f32 colourBonus = 1;
        u32 damageFlags = 0;
    };
    struct Burst {
        MissileImpact impact;
        std::string_view tree;
        std::string_view sound;
        EffectTrees::Setting setting;
    };

    /** Preserve native fallback artwork for zero/out-of-range kinds. This does
     * not choose a gameplay colour for an unspecified potion. */
    static const Look& look(s32 kind);
    static Burst burst(const Context& context);
    /** StartShieldFX scales only the footprint; the authored dome keeps its full height. */
    static Vec3 shieldScale(f32 radius);
    /** Native bottle launch, with no model/archive/texture bindings. Its sound
     * and burst occur on impact, not at this release. Negative held ticks retain
     * native's uncharged throw; charge otherwise counts native wind-up ticks. */
    static MissileLaunch throwPotion(const Context& context, s32 heldTicks);
};
} // namespace gdl::game
