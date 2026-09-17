#pragma once

#include <array>
#include <string>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/** One particle template as a manifest carries it: the letter a marker names it by, the
 * built-in preset it starts from, its flag bits with the mask of those it sets, the mask of
 * the fields it fills, and the fields in seconds, degrees and units a second. */
struct ParticleTemplate {
    /** The fields `enables` may name. */
    static constexpr u32 kPreset = 0x1;
    static constexpr u32 kMaxParticles = 0x2;
    static constexpr u32 kMaxDirections = 0x4;
    static constexpr u32 kMaxPositions = 0x8;
    static constexpr u32 kEmitterLife = 0x10;
    static constexpr u32 kParticleLife = 0x20;
    static constexpr u32 kAngle = 0x40;
    static constexpr u32 kDirection = 0x80;
    static constexpr u32 kVolume = 0x100;
    static constexpr u32 kRate = 0x200;
    static constexpr u32 kRateRandom = 0x400;
    static constexpr u32 kGravity = 0x800;
    static constexpr u32 kDrag = 0x1000;
    static constexpr u32 kSpeed = 0x2000;
    static constexpr u32 kTexture = 0x4000;
    static constexpr u32 kTextureCount = 0x8000;
    static constexpr u32 kColor = 0x10000;
    static constexpr u32 kAlpha = 0x20000;
    static constexpr u32 kWidth = 0x40000;
    static constexpr u32 kDelay = 0x80000;
    /** The flag bits, each set only where `flagMask` names it. */
    static constexpr u32 kDynamic = 0x1;       ///< positions follow the marker each frame
    static constexpr u32 kOneShot = 0x2;       ///< everything at once
    static constexpr u32 kForever = 0x4;       ///< the emission repeats
    static constexpr u32 kDefaultGravity = 0x8;
    static constexpr u32 kDefaultDrag = 0x10;
    static constexpr u32 kMultiply = 0x20;
    static constexpr u32 kFrameAdd = 0x40;
    static constexpr u32 kAdditive = 0x80; ///< added onto the frame
    static constexpr u32 kSorted = 0x200;
    static constexpr u32 kNoDepthTest = 0x400;
    static constexpr u32 kNoDepthWrite = 0x800;

    char id = 0;
    u32 preset = 0;
    u32 flags = 0;
    u32 flagMask = 0;
    u32 enables = 0;
    s32 maxParticles = 0;
    u32 maxDirections = 0;
    u32 maxPositions = 0;
    std::array<f32, 2> emitterLife{0.0f, 0.0f};
    std::array<f32, 2> particleLife{0.0f, 0.0f};
    f32 angle = 0.0f;
    s32 textureCount = 0;
    std::string texture;
    Vec3 direction{0.0f, 0.0f, 0.0f};
    Vec3 volume{0.0f, 0.0f, 0.0f};
    std::array<f32, 4> rate{0.0f, 0.0f, 0.0f, 0.0f};
    f32 rateRandom = 0.0f;
    f32 gravity = 0.0f;
    f32 drag = 0.0f;
    f32 speed = 0.0f;
    std::array<u32, 4> rgba{0, 0, 0, 0};
    std::array<f32, 4> width{0.0f, 0.0f, 0.0f, 0.0f};
    f32 delay = 0.0f;

    bool sets(u32 field) const { return (enables & field) != 0; }
    /** Whether the template decides a flag, and what it decides. */
    bool decides(u32 flag) const { return (flagMask & flag) != 0; }
    bool flag(u32 bit) const { return (flags & bit) != 0; }
};

} // namespace gdl
