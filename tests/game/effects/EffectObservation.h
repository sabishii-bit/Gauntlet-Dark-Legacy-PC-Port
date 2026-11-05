#pragma once

#include <optional>
#include <span>
#include <string>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/ParticleField.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeParticles.h"

#include "game/effects/EffectTrees.h"

namespace gdl::test {

/** What one playing effect shows: its tree, how it was set going and what is left of it, where
 * it stands, its clock, trails and particles. Two runs are compared through it; it is never
 * what a rewind restores. */
struct EffectObservation {
    std::string name;
    bool tree = false;
    u32 continuity = 1;
    game::EffectTrees::Setting setting; ///< seconds is what is left, loop the repeat now
    Vec3 position{0};
    bool timed = false;
    bool retiring = false;
    f32 lived = 0;
    AnimationPlayer::Checkpoint animation;
    ParticleField::Checkpoint trails;
    TreeParticles::Checkpoint particles;
    std::optional<Mat4> attachment;
    std::optional<Vec3> flightDirection;
    std::optional<u32> particleTextureSlot;
    bool operator==(const EffectObservation&) const = default;
};

/** What effect number `id` of `effects` shows now; nothing when it is not playing. */
inline std::optional<EffectObservation> observeEffect(const game::EffectTrees& effects, u32 id) {
    for (usize index = 0; index < effects.count(); ++index) {
        const auto& effect = effects.effect(index);
        if (effect.id != id) {
            continue;
        }
        EffectObservation seen;
        seen.name = effect.name;
        seen.tree = effect.tree != nullptr;
        seen.continuity = effect.continuity;
        auto& setting = seen.setting;
        setting.scale = effect.scale;
        setting.stretch = effect.stretch;
        setting.fadeSeconds = effect.fadeSeconds;
        setting.yaw = effect.yaw;
        setting.velocity = effect.velocity;
        setting.seconds = effect.secondsLeft;
        setting.loop = effect.repeats;
        setting.persistent = effect.persistent;
        setting.emitParticles = effect.emitParticles;
        setting.missingObjectsAreEmpty = effect.missingObjectsAreEmpty;
        setting.additive = effect.additive;
        setting.then = effect.then;
        setting.morphIn = effect.morphIn;
        setting.holdForMorph = effect.holdForMorph;
        setting.shrinks = effect.shrinks;
        setting.unlit = effect.unlit;
        setting.depthWrite = effect.depthWrite;
        setting.tint = effect.tint;
        setting.alpha = effect.alpha;
        setting.playbackRate = effect.playbackRate;
        setting.light = effect.light;
        seen.position = effect.position;
        seen.timed = effect.timed;
        seen.retiring = effect.retiring;
        seen.lived = effect.lived;
        seen.animation = effect.player.checkpoint();
        seen.trails = effect.trails.checkpoint();
        seen.particles = effect.particles.checkpoint();
        seen.attachment = effect.attachment;
        seen.flightDirection = effect.flightDirection;
        seen.particleTextureSlot = effect.particleTextureSlot;
        return seen;
    }
    return std::nullopt;
}

/** The clock of the texture bank `effects` shares among the effects of `archive` with
 * `lenders`; nothing before one of them has started. */
inline std::optional<TextureAnimator::Checkpoint>
observeTextureClock(const game::EffectTrees& effects, const ItemArchive& archive,
                    std::span<TextureSet* const> lenders = {}) {
    const TextureAnimator* bank = effects.textureAnimator(&archive, lenders);
    if (bank == nullptr) {
        return std::nullopt;
    }
    return bank->checkpoint();
}

} // namespace gdl::test
