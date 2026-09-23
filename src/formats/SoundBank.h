#pragma once

#include <span>
#include <string>
#include <vector>

#include "engine/codec/DspAdpcm.h"
#include "engine/core/Types.h"

namespace gdl::formats {

/** One ADPCM sample of a bank, still encoded. */
struct BankSample {
    std::string name;
    u32 sampleRate = 0;
    u32 sampleCount = 0;
    bool loops = false;
    u32 loopStart = 0; ///< in samples
    u32 loopEnd = 0;
    DspAdpcmCoefficients coefficients{};
    std::vector<u8> adpcm;
};

/** One step of a sound: which sample plays, and how the sequence continues after it. */
struct SoundStep {
    u16 sample = 0;
    bool loopStart = false; ///< a later step can jump back here
    bool loopBack = false;  ///< after this sample, continue from the nearest loop start
};

/** One playable sound: a sequence of samples plus its mixing parameters. */
struct SoundCall {
    std::vector<SoundStep> steps;
    u16 volume = 0;   ///< 0..127
    u16 duck = 0;     ///< how much other voices are attenuated while this plays
    u16 priority = 0; ///< raw priority word
};

/** A sound bank file: the calls the game triggers by sound id, and the samples they play. */
struct SoundBank {
    static constexpr u16 kMaxVolume = 127;

    std::vector<SoundCall> calls;
    std::vector<BankSample> samples;

    /** Parses the big-endian bank; throws FormatError. */
    static SoundBank parse(std::span<const u8> file);
};

/** Decodes a sample to 16-bit PCM at its own rate. */
std::vector<s16> decodeBankSample(const BankSample& sample);

} // namespace gdl::formats
