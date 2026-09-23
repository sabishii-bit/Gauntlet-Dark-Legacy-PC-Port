#include "formats/WavWriter.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/core/Assert.h"

namespace gdl::formats {

namespace {

void putU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    putU16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
    putU16(out, static_cast<std::uint16_t>(value >> 16U));
}

void putTag(std::vector<std::uint8_t>& out, std::string_view tag) {
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(tag[i]));
    }
}

constexpr std::uint16_t kPcmFormat = 1;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr std::uint32_t kFormatChunkSize = 16;

} // namespace

std::vector<std::uint8_t> encodeWav(std::span<const std::int16_t> samples, std::uint32_t sampleRate,
                                    std::uint32_t channels) {
    GDL_VERIFY(channels > 0 && sampleRate > 0, "encodeWav needs a channel count and a rate");
    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (kBitsPerSample / 8));
    std::vector<std::uint8_t> out;
    out.reserve(44 + dataBytes);
    putTag(out, "RIFF");
    putU32(out, 36 + dataBytes);
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    putU32(out, kFormatChunkSize);
    putU16(out, kPcmFormat);
    putU16(out, static_cast<std::uint16_t>(channels));
    putU32(out, sampleRate);
    putU32(out, sampleRate * blockAlign);
    putU16(out, blockAlign);
    putU16(out, kBitsPerSample);
    putTag(out, "data");
    putU32(out, dataBytes);
    for (const std::int16_t sample : samples) {
        putU16(out, static_cast<std::uint16_t>(sample));
    }
    return out;
}

} // namespace gdl::formats
