#include "formats/WavWriter.h"

#include <string_view>

#include "engine/core/Assert.h"

namespace gdl::formats {

namespace {

void putU16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value & 0xFFU));
    out.push_back(static_cast<u8>(value >> 8U));
}

void putU32(std::vector<u8>& out, u32 value) {
    putU16(out, static_cast<u16>(value & 0xFFFFU));
    putU16(out, static_cast<u16>(value >> 16U));
}

void putTag(std::vector<u8>& out, std::string_view tag) {
    for (usize i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(tag[i]));
    }
}

constexpr u16 kPcmFormat = 1;
constexpr u16 kBitsPerSample = 16;
constexpr u32 kFormatChunkSize = 16;

} // namespace

std::vector<u8> encodeWav(std::span<const s16> samples, u32 sampleRate, u32 channels) {
    GDL_VERIFY(channels > 0 && sampleRate > 0, "encodeWav needs a channel count and a rate");
    const auto dataBytes = static_cast<u32>(samples.size() * sizeof(s16));
    const u16 blockAlign = static_cast<u16>(channels * (kBitsPerSample / 8));
    std::vector<u8> out;
    out.reserve(44 + dataBytes);
    putTag(out, "RIFF");
    putU32(out, 36 + dataBytes);
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    putU32(out, kFormatChunkSize);
    putU16(out, kPcmFormat);
    putU16(out, static_cast<u16>(channels));
    putU32(out, sampleRate);
    putU32(out, sampleRate * blockAlign);
    putU16(out, blockAlign);
    putU16(out, kBitsPerSample);
    putTag(out, "data");
    putU32(out, dataBytes);
    for (const s16 sample : samples) {
        putU16(out, static_cast<u16>(sample));
    }
    return out;
}

} // namespace gdl::formats
