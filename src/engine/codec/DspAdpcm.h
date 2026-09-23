#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gdl {

/** The 16 predictor coefficients (8 pairs) of one GameCube DSP-ADPCM channel. */
using DspAdpcmCoefficients = std::array<std::int16_t, 16>;

/** Reads a big-endian coefficient table as stored in stream headers. */
DspAdpcmCoefficients readDspAdpcmCoefficients(std::span<const std::uint8_t> bytes);

/**
 * Decoder for GameCube DSP-ADPCM: 8-byte frames of one header byte (coefficient pair index
 * and scale) followed by 14 signed 4-bit residuals. Keeps the two-sample history between calls.
 */
class DspAdpcmDecoder {
public:
    static constexpr std::size_t kFrameBytes = 8;
    static constexpr std::size_t kSamplesPerFrame = 14;

    explicit DspAdpcmDecoder(const DspAdpcmCoefficients& coefficients);

    /** Decodes whole frames from `frames` (its size must be a multiple of 8), appending samples. */
    void decode(std::span<const std::uint8_t> frames, std::vector<std::int16_t>& out);

    void reset();

private:
    DspAdpcmCoefficients m_coefficients;
    std::int32_t m_history1 = 0;
    std::int32_t m_history2 = 0;
};

} // namespace gdl
