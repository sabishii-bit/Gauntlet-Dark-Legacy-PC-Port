#include "engine/codec/DspAdpcm.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "engine/core/Error.h"

namespace gdl {

namespace {

constexpr std::int32_t kFixedPointShift = 11;
constexpr std::int32_t kRounding = 1024;
constexpr std::int32_t kSampleMin = -32768;
constexpr std::int32_t kSampleMax = 32767;

/** Sign-extends a 4-bit residual. */
constexpr std::int32_t signedNibble(std::uint32_t nibble) {
    return nibble >= 8 ? static_cast<std::int32_t>(nibble) - 16 : static_cast<std::int32_t>(nibble);
}

} // namespace

DspAdpcmCoefficients readDspAdpcmCoefficients(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 32) {
        throw FormatError("DSP-ADPCM coefficient table is truncated");
    }
    DspAdpcmCoefficients coefficients{};
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        coefficients[i] =
            static_cast<std::int16_t>((std::uint32_t{bytes[i * 2]} << 8U) | bytes[i * 2 + 1]);
    }
    return coefficients;
}

DspAdpcmDecoder::DspAdpcmDecoder(const DspAdpcmCoefficients& coefficients)
    : m_coefficients(coefficients) {}

void DspAdpcmDecoder::decode(std::span<const std::uint8_t> frames, std::vector<std::int16_t>& out) {
    if (frames.size() % kFrameBytes != 0) {
        throw FormatError("DSP-ADPCM data is not a whole number of frames");
    }
    out.reserve(out.size() + (frames.size() / kFrameBytes) * kSamplesPerFrame);
    for (std::size_t offset = 0; offset < frames.size(); offset += kFrameBytes) {
        const std::span<const std::uint8_t> frame = frames.subspan(offset, kFrameBytes);
        const std::uint32_t header = frame[0];
        const std::size_t pair = std::min<std::size_t>(header >> 4U, 7);
        const std::int32_t scale = 1 << (header & 0x0FU);
        const std::int32_t coefficient1 = m_coefficients[pair * 2];
        const std::int32_t coefficient2 = m_coefficients[pair * 2 + 1];

        for (std::size_t i = 1; i < kFrameBytes; ++i) {
            for (const std::uint32_t nibble :
                 {std::uint32_t{frame[i]} >> 4U, std::uint32_t{frame[i]} & 0x0FU}) {
                const std::int32_t residual = (signedNibble(nibble) * scale) << kFixedPointShift;
                const std::int32_t predicted =
                    coefficient1 * m_history1 + coefficient2 * m_history2;
                const std::int32_t sample = std::clamp(
                    (residual + predicted + kRounding) >> kFixedPointShift, kSampleMin, kSampleMax);
                out.push_back(static_cast<std::int16_t>(sample));
                m_history2 = m_history1;
                m_history1 = sample;
            }
        }
    }
}

void DspAdpcmDecoder::reset() {
    m_history1 = 0;
    m_history2 = 0;
}

} // namespace gdl
