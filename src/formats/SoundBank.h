#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/codec/DspAdpcm.h"

namespace gdl::formats {

/** One ADPCM sample of a bank, still encoded. */
struct BankSample {
    std::string name;
    std::uint32_t sampleRate = 0;
    std::uint32_t sampleCount = 0;
    bool loops = false;
    std::uint32_t loopStart = 0; ///< in samples
    std::uint32_t loopEnd = 0;
    DspAdpcmCoefficients coefficients{};
    std::vector<std::uint8_t> adpcm;
};

/** One step of a sound: which sample plays, and how the sequence continues after it. */
struct SoundStep {
    std::uint16_t sample = 0;
    bool loopStart = false; ///< a later step can jump back here
    bool loopBack = false;  ///< after this sample, continue from the nearest loop start
};

/** One playable sound: a sequence of samples plus its mixing parameters. */
struct SoundCall {
    std::vector<SoundStep> steps;
    std::uint16_t volume = 0;   ///< 0..127
    std::uint16_t duck = 0;     ///< how much other voices are attenuated while this plays
    std::uint16_t priority = 0; ///< raw priority word
};

/** A sound bank file: the calls the game triggers by sound id, and the samples they play. */
struct SoundBank {
    static constexpr std::uint16_t kMaxVolume = 127;

    std::vector<SoundCall> calls;
    std::vector<BankSample> samples;

    /** Parses the big-endian bank; throws FormatError. */
    static SoundBank parse(std::span<const std::uint8_t> file);
};

/** Decodes a sample to 16-bit PCM at its own rate. */
std::vector<std::int16_t> decodeBankSample(const BankSample& sample);

} // namespace gdl::formats
