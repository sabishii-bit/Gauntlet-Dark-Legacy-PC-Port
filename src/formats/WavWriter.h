#pragma once

#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

/** Encodes interleaved 16-bit PCM as a RIFF WAVE file. */
std::vector<u8> encodeWav(std::span<const s16> samples, u32 sampleRate, u32 channels);

} // namespace gdl::formats
