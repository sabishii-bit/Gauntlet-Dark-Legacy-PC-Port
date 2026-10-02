#pragma once

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::test {

struct NativeSoundSample {
    u32 sampleRate = 48000;
    std::vector<s16> samples;
};

/** Writes a synthetic VBK and merges its names into the adjacent AUDATPS2.ROM.
 * The test-only JSON declaration contains a sounds array with name/id/duration,
 * volume/duck/priority and sequence sample/loopStart/loopBack fields.
 * PCM is mono, quantized to DSP's four-bit residuals with a zero predictor;
 * use exactly representable tones when asserting precise mixer amplitudes. */
void writeNativeSoundBank(const std::filesystem::path& bankPath, std::string_view soundsJson,
                          std::span<const NativeSoundSample> samples);

} // namespace gdl::test
