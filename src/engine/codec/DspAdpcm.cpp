#include "engine/codec/DspAdpcm.h"

#include <algorithm>

#include "engine/core/Error.h"
#include "engine/core/Types.h"

namespace gdl {

namespace {

constexpr s32 kFixedPointShift = 11;
constexpr s32 kRounding = 1024;
constexpr s32 kSampleMin = -32768;
constexpr s32 kSampleMax = 32767;

/** Sign-extends a 4-bit residual. */
constexpr s32 signedNibble(u32 nibble) {
    return nibble >= 8 ? static_cast<s32>(nibble) - 16 : static_cast<s32>(nibble);
}

} // namespace

DspAdpcmCoefficients readDspAdpcmCoefficients(std::span<const u8> bytes) {
    if (bytes.size() < 32) {
        throw FormatError("DSP-ADPCM coefficient table is truncated");
    }
    DspAdpcmCoefficients coefficients{};
    for (usize i = 0; i < coefficients.size(); ++i) {
        coefficients[i] = static_cast<s16>((u32{bytes[i * 2]} << 8U) | bytes[i * 2 + 1]);
    }
    return coefficients;
}

DspAdpcmDecoder::DspAdpcmDecoder(const DspAdpcmCoefficients& coefficients)
    : m_coefficients(coefficients) {}

void DspAdpcmDecoder::decode(std::span<const u8> frames, std::vector<s16>& out) {
    if (frames.size() % kFrameBytes != 0) {
        throw FormatError("DSP-ADPCM data is not a whole number of frames");
    }
    out.reserve(out.size() + (frames.size() / kFrameBytes) * kSamplesPerFrame);
    for (usize offset = 0; offset < frames.size(); offset += kFrameBytes) {
        const std::span<const u8> frame = frames.subspan(offset, kFrameBytes);
        const u32 header = frame[0];
        const usize pair = std::min<usize>(header >> 4U, 7);
        const s32 scale = 1 << (header & 0x0FU);
        const s32 coefficient1 = m_coefficients[pair * 2];
        const s32 coefficient2 = m_coefficients[pair * 2 + 1];

        for (usize i = 1; i < kFrameBytes; ++i) {
            for (const u32 nibble : {u32{frame[i]} >> 4U, u32{frame[i]} & 0x0FU}) {
                const s32 residual = (signedNibble(nibble) * scale) << kFixedPointShift;
                const s32 predicted = coefficient1 * m_history1 + coefficient2 * m_history2;
                const s32 sample = std::clamp(
                    (residual + predicted + kRounding) >> kFixedPointShift, kSampleMin, kSampleMax);
                out.push_back(static_cast<s16>(sample));
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
