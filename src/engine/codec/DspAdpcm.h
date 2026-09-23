#pragma once

#include <array>
#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/** The 16 predictor coefficients (8 pairs) of one GameCube DSP-ADPCM channel. */
using DspAdpcmCoefficients = std::array<s16, 16>;

/** Reads a big-endian coefficient table as stored in stream headers. */
DspAdpcmCoefficients readDspAdpcmCoefficients(std::span<const u8> bytes);

/**
 * Decoder for GameCube DSP-ADPCM: 8-byte frames of one header byte (coefficient pair index
 * and scale) followed by 14 signed 4-bit residuals. Keeps the two-sample history between calls.
 */
class DspAdpcmDecoder {
public:
    static constexpr usize kFrameBytes = 8;
    static constexpr usize kSamplesPerFrame = 14;

    explicit DspAdpcmDecoder(const DspAdpcmCoefficients& coefficients);

    /** Decodes whole frames from `frames` (its size must be a multiple of 8), appending samples. */
    void decode(std::span<const u8> frames, std::vector<s16>& out);

    void reset();

private:
    DspAdpcmCoefficients m_coefficients;
    s32 m_history1 = 0;
    s32 m_history2 = 0;
};

} // namespace gdl
