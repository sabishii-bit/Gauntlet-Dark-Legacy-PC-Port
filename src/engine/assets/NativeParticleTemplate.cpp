#include "engine/assets/NativeParticleTemplate.h"

namespace gdl {

ParticleTemplate nativeParticle(const formats::ParticleTemplateRecord& source) {
    ParticleTemplate result;
    result.id = source.id;
    result.preset = source.preset;
    result.flags = source.flags;
    result.flagMask = source.flagMask;
    result.enables = source.enables;
    result.maxParticles = source.maxParticles;
    result.maxDirections = source.maxDirections;
    result.maxPositions = source.maxPositions;
    result.emitterLife = source.emitterLife;
    result.particleLife = source.particleLife;
    result.angle = source.angle;
    result.textureCount = source.textureCount;
    result.texture = source.texture;
    result.direction = source.direction;
    result.volume = source.volume;
    result.rate = source.rate;
    result.rateRandom = source.rateRandom;
    result.gravity = source.gravity;
    result.drag = source.drag;
    result.speed = source.speed;
    result.rgba = source.rgba;
    result.width = source.width;
    result.delay = source.delay;
    return result;
}

} // namespace gdl
