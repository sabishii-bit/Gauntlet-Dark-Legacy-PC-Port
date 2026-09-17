#pragma once

#include <array>
#include <span>
#include <string>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

/** One particle template as a level or archive file stores it: the letter a marker names
 * it by, the built-in preset it starts from, its flag bits with the mask of those it sets,
 * the mask of fields it fills, and the fields themselves in the file's units (seconds,
 * degrees, units a second). */
struct ParticleTemplateRecord {
    static constexpr usize kSize = 0x138;

    char id = 0;
    u16 preset = 0;
    u32 flags = 0;
    u32 flagMask = 0;
    u32 enables = 0;
    s32 maxParticles = 0;
    u32 maxDirections = 0;
    u32 maxPositions = 0;
    std::array<f32, 2> emitterLife{0.0f, 0.0f}; ///< seconds emitting, then fading
    std::array<f32, 2> particleLife{0.0f, 0.0f};
    f32 angle = 0.0f; ///< the emission cone in degrees
    s32 textureCount = 0;
    std::string texture;
    Vec3 direction{0.0f, 0.0f, 0.0f};
    Vec3 volume{0.0f, 0.0f, 0.0f};
    std::array<f32, 4> rate{0.0f, 0.0f, 0.0f, 0.0f}; ///< particles a second over each phase
    f32 rateRandom = 0.0f;
    f32 gravity = 0.0f;
    f32 drag = 0.0f;
    f32 speed = 0.0f;
    std::array<u32, 4> rgba{0, 0, 0, 0}; ///< packed AARRGGBB at birth, life's end, fade's start, death
    std::array<f32, 4> width{0.0f, 0.0f, 0.0f, 0.0f};
    f32 delay = 0.0f;
};

/** Reads one record of kSize bytes; throws FormatError when shorter. */
ParticleTemplateRecord readParticleTemplate(std::span<const u8> record);

} // namespace gdl::formats
