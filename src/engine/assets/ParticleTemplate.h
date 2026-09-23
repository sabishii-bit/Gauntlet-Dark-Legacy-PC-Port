#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "engine/math/Math.h"

namespace gdl {

/** One particle template as a manifest carries it: the letter a marker names it by, the
 * built-in preset it starts from, its flag bits with the mask of those it sets, the mask of
 * the fields it fills, and the fields in seconds, degrees and units a second. */
struct ParticleTemplate {
    /** The fields `enables` may name. */
    static constexpr std::uint32_t kPreset = 0x1;
    static constexpr std::uint32_t kMaxParticles = 0x2;
    static constexpr std::uint32_t kMaxDirections = 0x4;
    static constexpr std::uint32_t kMaxPositions = 0x8;
    static constexpr std::uint32_t kEmitterLife = 0x10;
    static constexpr std::uint32_t kParticleLife = 0x20;
    static constexpr std::uint32_t kAngle = 0x40;
    static constexpr std::uint32_t kDirection = 0x80;
    static constexpr std::uint32_t kVolume = 0x100;
    static constexpr std::uint32_t kRate = 0x200;
    static constexpr std::uint32_t kRateRandom = 0x400;
    static constexpr std::uint32_t kGravity = 0x800;
    static constexpr std::uint32_t kDrag = 0x1000;
    static constexpr std::uint32_t kSpeed = 0x2000;
    static constexpr std::uint32_t kTexture = 0x4000;
    static constexpr std::uint32_t kTextureCount = 0x8000;
    static constexpr std::uint32_t kColor = 0x10000;
    static constexpr std::uint32_t kAlpha = 0x20000;
    static constexpr std::uint32_t kWidth = 0x40000;
    static constexpr std::uint32_t kDelay = 0x80000;
    /** The flag bits, each set only where `flagMask` names it. */
    static constexpr std::uint32_t kDynamic = 0x1; ///< positions follow the marker each frame
    static constexpr std::uint32_t kOneShot = 0x2; ///< everything at once
    static constexpr std::uint32_t kForever = 0x4; ///< the emission repeats
    static constexpr std::uint32_t kDefaultGravity = 0x8;
    static constexpr std::uint32_t kDefaultDrag = 0x10;
    static constexpr std::uint32_t kMultiply = 0x20;
    static constexpr std::uint32_t kFrameAdd = 0x40;
    static constexpr std::uint32_t kAdditive = 0x80; ///< added onto the frame
    static constexpr std::uint32_t kSorted = 0x200;
    static constexpr std::uint32_t kNoDepthTest = 0x400;
    static constexpr std::uint32_t kNoDepthWrite = 0x800;

    char id = 0;
    std::uint32_t preset = 0;
    std::uint32_t flags = 0;
    std::uint32_t flagMask = 0;
    std::uint32_t enables = 0;
    std::int32_t maxParticles = 0;
    std::uint32_t maxDirections = 0;
    std::uint32_t maxPositions = 0;
    std::array<float, 2> emitterLife{0.0f, 0.0f};
    std::array<float, 2> particleLife{0.0f, 0.0f};
    float angle = 0.0f;
    std::int32_t textureCount = 0;
    std::string texture;
    Vec3 direction{0.0f, 0.0f, 0.0f};
    Vec3 volume{0.0f, 0.0f, 0.0f};
    std::array<float, 4> rate{0.0f, 0.0f, 0.0f, 0.0f};
    float rateRandom = 0.0f;
    float gravity = 0.0f;
    float drag = 0.0f;
    float speed = 0.0f;
    std::array<std::uint32_t, 4> rgba{0, 0, 0, 0};
    std::array<float, 4> width{0.0f, 0.0f, 0.0f, 0.0f};
    float delay = 0.0f;

    bool sets(std::uint32_t field) const { return (enables & field) != 0; }
    /** Whether the template decides a flag, and what it decides. */
    bool decides(std::uint32_t flag) const { return (flagMask & flag) != 0; }
    bool flag(std::uint32_t bit) const { return (flags & bit) != 0; }
};

} // namespace gdl
