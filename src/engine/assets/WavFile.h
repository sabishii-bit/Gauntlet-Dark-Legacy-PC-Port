#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/** Interleaved 16-bit PCM read from a RIFF WAVE file. */
struct WavData {
    u32 sampleRate = 0;
    u32 channels = 0;
    std::vector<s16> samples;

    usize frames() const { return channels == 0 ? 0 : samples.size() / channels; }
};

/** Parses a 16-bit PCM WAVE file; throws FormatError for anything else. */
WavData decodeWav(std::span<const u8> bytes);

/** Reads and parses a WAVE file; throws FileError or FormatError. */
WavData loadWav(const std::filesystem::path& path);

} // namespace gdl
