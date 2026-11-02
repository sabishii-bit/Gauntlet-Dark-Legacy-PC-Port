#include "game/party/TurboEmission.h"

#include <algorithm>
#include <utility>

#include "engine/math/PortableMath.h"

#include "game/combat/EffectFlags.h"
#include "game/effects/DynamicLights.h"

namespace gdl::game {
f32 TurboEmission::effectSeconds(const MoveEffect& effect, const TreeInfo* tree) {
    if (tree == nullptr) {
        return 0;
    }
    if (effect.lifetime > 0) {
        return effect.lifetime;
    }
    if (tree->sequences.empty()) {
        return 0;
    }
    const auto& sequence = tree->sequences.front();
    constexpr s32 kEmptyFrames = 30;
    const auto frames = static_cast<f32>(sequence.frames > 0 ? sequence.frames : kEmptyFrames);
    const f32 rate = sequence.frameRate > 0 ? static_cast<f32>(sequence.frameRate)
                                            : AnimationPlayer::kDefaultRate;
    return frames * rate * AnimationPlayer::kRateUnit;
}

TurboEmission::Particle TurboEmission::particle(const MoveEffect& effect) {
    // PsfxDoParticle starts a default emitter. Its named setters configure a
    // spherical cone and particle life/fade, not one of the authored presets.
    Particle result;
    auto& descriptor = result.descriptor;
    descriptor.texture = effect.tree;
    descriptor.emitFrames =
        effect.lifetime < 0
            ? ParticleDescriptor::kEndless
            : static_cast<u32>(
                  std::clamp(effect.lifetime * ParticleDescriptor::kFrameRate, 1.0f, 65535.0f));
    descriptor.fadeFrames = 1; // 0.034 seconds, truncated at 30 Hz
    descriptor.angle = ParticleDescriptor::kSphere;
    descriptor.rate.fill(effect.radius);
    descriptor.speed = static_cast<f32>(effect.alphaMod) * 0.01f / ParticleDescriptor::kFrameRate;
    const bool shortLife =
        (effect.flags & effect_flags::kParticleMask) == effect_flags::kShortParticle;
    descriptor.particleLife = shortLife ? 6 : 15;
    descriptor.particleFade = shortLife ? 0 : 15;
    descriptor.red = descriptor.green = descriptor.blue = descriptor.alpha =
        ParticleEnvelope{255, 255, 255, 255};
    const f32 width = 0.5f * effect.scale;
    descriptor.width = ParticleEnvelope{width, width, width, width};
    result.node = effect.sound;
    result.offset = effect.offset;
    result.previousEffect = (effect.flags & effect_flags::kUntilNextMove) != 0;
    result.partnerFallback = (effect.flags & effect_flags::kOtherParent) != 0;
    return result;
}

std::optional<TurboEmission::Strike> TurboEmission::prepare(const ClassStats& stats, s32 row,
                                                            const Context& context, f32 meleeDamage,
                                                            f32 effectSeconds) {
    if (row < 0 || static_cast<usize>(row) >= stats.moveStrikes.size()) {
        return std::nullopt;
    }
    const auto& strike = stats.moveStrikes[static_cast<usize>(row)];
    Strike result;
    result.row = row;
    result.volume = strike;
    result.effectSeconds = effectSeconds;
    if (strike.effect >= 0 && static_cast<usize>(strike.effect) < stats.moveEffects.size()) {
        result.volume.offset += stats.moveEffects[static_cast<usize>(strike.effect)].offset;
    }
    if (strike.harms() && MoveStrikes::damageOf(strike, meleeDamage) > 0) {
        MoveStrikes carriers;
        carriers.start(result.volume, 0, context.position, context.facing, meleeDamage,
                       effectSeconds);
        result.carrier = carriers.strike(0);
    }
    const auto* carrier = result.carrier ? &*result.carrier : nullptr;
    const Vec3 origin = MoveStrikes::originOf(strike, context.position, context.facing);
    const Vec3 side{context.facing.z, 0, -context.facing.x};
    usize followed = 0;
    for (s32 index = strike.effect;
         index >= 0 && static_cast<usize>(index) < stats.moveEffects.size() &&
         followed < stats.moveEffects.size();
         index = stats.moveEffects[static_cast<usize>(index)].next, ++followed) {
        const auto& effect = stats.moveEffects[static_cast<usize>(index)];
        Effect emission;
        emission.row = index;
        emission.authored = effect;
        emission.primary = index == strike.effect;
        emission.shake = (effect.flags & effect_flags::kShakes) != 0;
        if (effect.particle()) {
            emission.particle = particle(effect);
            result.effects.push_back(std::move(emission));
            continue;
        }
        constexpr u32 kOtherParents = effect_flags::kOnFloor | effect_flags::kDetached |
                                      effect_flags::kAtInitialRoot | effect_flags::kRootParent |
                                      effect_flags::kUntilNextMove;
        // Stationary area trees follow the first posed node. Saved matrices,
        // floor placement and stage/effect parents retain separate SFXX policies.
        if (!context.detached && strike.type != MoveStrike::kFlies && strike.speed == 0 &&
            (effect.flags & kOtherParents) == 0) {
            Attachment attachment;
            attachment.carrier = emission.primary && carrier != nullptr;
            attachment.bodyParent = (effect.flags & effect_flags::kBodyParent) != 0;
            if ((effect.flags & effect_flags::kOtherParent) != 0 && context.partnerAvailable) {
                attachment.actor = Actor::Partner;
            }
            const Vec3 offset = effect.offset + (attachment.bodyParent ? Vec3{0} : strike.offset);
            attachment.local = glm::translate(Mat4{1}, offset);
            if (emission.primary) {
                attachment.local = portable::rotate(attachment.local, strike.angle, Vec3{0, 1, 0});
            }
            emission.attachment = attachment;
        }
        auto& setting = emission.setting;
        setting.scale = effect.scale;
        setting.tint = effect.tint();
        setting.yaw = portable::atan2(context.facing.x, context.facing.z) + strike.angle;
        setting.seconds = effect.lifetime;
        setting.shrinks = (effect.flags & effect_flags::kShrinks) != 0 && strike.hitEffect < 0;
        emission.carriesFlight = emission.primary && carrier != nullptr && carrier->flies;
        const bool loops = strike.loopEffect >= 0 &&
                           static_cast<usize>(strike.loopEffect) < stats.moveEffects.size();
        if (emission.carriesFlight) {
            setting.velocity = carrier->facing * carrier->speed;
            setting.seconds = carrier->secondsLeft;
            if (loops) {
                setting.then = stats.moveEffects[static_cast<usize>(strike.loopEffect)].tree;
                setting.morphIn = carrier->secondsLeft - strike.maxTime;
                setting.holdForMorph = (strike.flags & MoveStrike::kWaitsForMorph) != 0;
            }
        } else if (emission.primary && carrier != nullptr && loops) {
            // PlyrSfxDoDamageSub morphs every harming row's effect into its loop effect once
            // it has played, for the row's time (psfx.c 650): the dwarf's charge flame stays
            // on the charger the whole ride.
            setting.then = stats.moveEffects[static_cast<usize>(strike.loopEffect)].tree;
            setting.seconds = effectSeconds + strike.maxTime;
            setting.morphIn = effectSeconds;
            setting.holdForMorph = (strike.flags & MoveStrike::kWaitsForMorph) != 0;
        }
        emission.position = origin + side * effect.offset.x + Vec3{0, effect.offset.y, 0} +
                            context.facing * effect.offset.z;
        if (emission.primary && carrier != nullptr) {
            // PlyrSfxDoDamageSub lights the primary effect at twice its reach.
            const f32 reach = strike.radius > 0 ? strike.radius : strike.hitRadius;
            setting.light =
                EffectTrees::Light{DynamicLights::ofClass(context.character),
                                   DynamicLights::kBlastRadiusScale * reach, !carrier->flies};
        }
        result.effects.push_back(std::move(emission));
    }
    return result;
}

TurboEmission::Volley TurboEmission::volley(PlayerMissiles::WeaponThrow weapon,
                                            const Vec3& direction, u32 soundFlags) {
    weapon.direction = direction;
    weapon.scale = 1;
    weapon.shots = 1;
    weapon.target.reset();
    return {weapon, soundFlags, false};
}
} // namespace gdl::game
