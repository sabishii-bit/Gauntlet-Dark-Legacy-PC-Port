#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace gdl::formats {

/** Encodes interleaved 16-bit PCM as a RIFF WAVE file. */
std::vector<std::uint8_t> encodeWav(std::span<const std::int16_t> samples, std::uint32_t sampleRate,
                                    std::uint32_t channels);

} // namespace gdl::formats
