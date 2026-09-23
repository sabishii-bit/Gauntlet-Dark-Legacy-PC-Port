#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "engine/math/Math.h"

namespace gdl::formats {

/** One particle template as a level or archive file stores it: the letter a marker names
 * it by, the built-in preset it starts from, its flag bits with the mask of those it sets,
 * the mask of fields it fills, and the fields themselves in the file's units (seconds,
 * degrees, units a second). */
struct ParticleTemplateRecord {
    static constexpr std::size_t kSize = 0x138;

    char id = 0;
    std::uint16_t preset = 0;
    std::uint32_t flags = 0;
    std::uint32_t flagMask = 0;
    std::uint32_t enables = 0;
    std::int32_t maxParticles = 0;
    std::uint32_t maxDirections = 0;
    std::uint32_t maxPositions = 0;
    std::array<float, 2> emitterLife{0.0f, 0.0f}; ///< seconds emitting, then fading
    std::array<float, 2> particleLife{0.0f, 0.0f};
    float angle = 0.0f; ///< the emission cone in degrees
    std::int32_t textureCount = 0;
    std::string texture;
    Vec3 direction{0.0f, 0.0f, 0.0f};
    Vec3 volume{0.0f, 0.0f, 0.0f};
    std::array<float, 4> rate{0.0f, 0.0f, 0.0f, 0.0f}; ///< particles a second over each phase
    float rateRandom = 0.0f;
    float gravity = 0.0f;
    float drag = 0.0f;
    float speed = 0.0f;
    std::array<std::uint32_t, 4> rgba{
        0, 0, 0, 0}; ///< packed AARRGGBB at birth, life's end, fade's start, death
    std::array<float, 4> width{0.0f, 0.0f, 0.0f, 0.0f};
    float delay = 0.0f;
};

/** Reads one record of kSize bytes; throws FormatError when shorter. */
ParticleTemplateRecord readParticleTemplate(std::span<const std::uint8_t> record);

} // namespace gdl::formats
