#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace gdl {

/** Interleaved 16-bit PCM read from a RIFF WAVE file. */
struct WavData {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::vector<std::int16_t> samples;

    std::size_t frames() const { return channels == 0 ? 0 : samples.size() / channels; }
};

/** Parses a 16-bit PCM WAVE file; throws FormatError for anything else. */
WavData decodeWav(std::span<const std::uint8_t> bytes);

/** Reads and parses a WAVE file; throws FileError or FormatError. */
WavData loadWav(const std::filesystem::path& path);

} // namespace gdl
