#include "game/party/PotionEmission.h"

#include <algorithm>
#include <array>

#include "game/effects/DynamicLights.h"

namespace gdl::game {
namespace {
constexpr std::array<PotionEmission::Look, 5> kPotions{{
    {"POT_RED_TW", "MP_FIRE", "S_POTION2"},
    {"POT_RED_TW", "MP_FIRE", "S_POTION2"},
    {"POT_BLU_TW", "MP_ELEC", "S_POTION1"},
    {"POT_YEL_TW", "MP_LIGHT", "S_POTION3"},
    {"POT_GRE_TW", "MP_ACID", "S_POTION4"},
}};
constexpr f32 kPotionToss = 5.0f;
constexpr f32 kPotionCharge = 1.5f;
constexpr f32 kThrownShare = 0.75f;
constexpr f32 kPotionLoft = 0.707f;
constexpr f32 kPotionHandHeight = 4.0f;
constexpr f32 kPotionHandReach = 2.0f;
} // namespace

const PotionEmission::Look& PotionEmission::look(s32 kind) {
    return kPotions[kind >= 0 && static_cast<usize>(kind) < kPotions.size()
                        ? static_cast<usize>(kind)
                        : 0];
}

Vec3 PotionEmission::shieldScale(f32 radius) {
    const f32 footprint = std::clamp(radius / 12.0f, 0.33f, 1.0f);
    return {footprint, 1.0f, footprint};
}

PotionEmission::Burst PotionEmission::burst(const Context& context) {
    Burst result;
    result.impact.position = context.position;
    result.impact.owner = context.owner;
    result.impact.potion = context.resolvedKind;
    result.impact.potency = context.power;
    result.impact.damage = kDamage * context.colourBonus;
    result.impact.flags = context.damageFlags;
    const Look& artwork = look(context.resolvedKind);
    result.tree = artwork.burst;
    result.sound = artwork.sound;
    result.setting.scale = std::min(kBurstPerPower * context.power, 1.0f);
    result.setting.light = EffectTrees::Light{DynamicLights::ofPotion(context.resolvedKind),
                                              DynamicLights::kMagicRadiusPerPower * context.power};
    return result;
}

MissileLaunch PotionEmission::throwPotion(const Context& context, s32 heldTicks) {
    MissileLaunch launch;
    launch.owner = context.owner;
    launch.direction = context.facing;
    launch.position =
        context.position + context.facing * kPotionHandReach + Vec3{0.0f, kPotionHandHeight, 0.0f};
    launch.velocity =
        Vec3{context.facing.x * kPotionLoft, kPotionLoft, context.facing.z * kPotionLoft} *
        (kPotionToss + kPotionCharge * static_cast<f32>(std::max(0, heldTicks)));
    launch.potion = context.resolvedKind;
    launch.potency = kThrownShare * context.power;
    launch.damage = kDamage * context.colourBonus;
    launch.flags = context.damageFlags;
    launch.spec = &MissileSpec::potion();
    launch.tree = look(context.resolvedKind).bottle;
    return launch;
}
} // namespace gdl::game
