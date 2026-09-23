#include "formats/SoundBank.h"

#include <cstddef>
#include <format>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr usize kHeaderSize = 20;
constexpr u32 kMagic = 0x4B4E4256; // "KNBV"
constexpr usize kParameterWords = 3;
constexpr u16 kLastStep = 0x8000;
constexpr u16 kLoopStartStep = 0x4000;
constexpr u16 kLoopBackStep = 0x2000;
constexpr u16 kSampleMask = 0x0FFF;
constexpr usize kVagHeaderSize = 48;
constexpr u32 kVagMagic = 0x56414770; // "VAGp"
constexpr u32 kVagWithDspHeader = 0x28;
constexpr usize kDspHeaderSize = 96;
constexpr usize kVagNameAt = 32;
constexpr usize kVagNameSize = 16;

u32 u32At(std::span<const u8> file, usize at) {
    return readU32BE(file, at);
}

u16 u16At(std::span<const u8> file, usize at) {
    return readU16BE(file, at);
}

} // namespace

SoundBank SoundBank::parse(std::span<const u8> file) {
    if (file.size() < kHeaderSize) {
        throw FormatError("sound bank is too small for its header");
    }
    if (u32At(file, 0) != kMagic) {
        throw FormatError("sound bank has no KNBV signature");
    }
    const u32 callBytes = u32At(file, 4);
    const u32 callCount = u32At(file, 12);
    const u32 sampleCount = u32At(file, 16);
    if (callBytes % 2 != 0 || callBytes > file.size() - kHeaderSize) {
        throw FormatError("sound bank call table lies outside the file");
    }

    SoundBank bank;
    const usize callWords = callBytes / 2;
    usize word = 0;
    for (u32 c = 0; c < callCount; ++c) {
        SoundCall call;
        bool ended = false;
        while (word < callWords && !ended) {
            const u16 value = u16At(file, kHeaderSize + word * 2);
            ++word;
            SoundStep step;
            step.sample = value & kSampleMask;
            step.loopStart = (value & kLoopStartStep) != 0;
            step.loopBack = (value & kLoopBackStep) != 0;
            ended = (value & kLastStep) != 0;
            call.steps.push_back(step);
        }
        if (!ended || word + kParameterWords > callWords) {
            throw FormatError(std::format("sound bank call {} is truncated", c));
        }
        call.volume = u16At(file, kHeaderSize + word * 2);
        call.duck = u16At(file, kHeaderSize + (word + 1) * 2);
        call.priority = u16At(file, kHeaderSize + (word + 2) * 2);
        word += kParameterWords;
        bank.calls.push_back(std::move(call));
    }

    usize at = kHeaderSize + callBytes;
    for (u32 s = 0; s < sampleCount; ++s) {
        if (at + kVagHeaderSize > file.size()) {
            throw FormatError(std::format("sound bank sample {} header is truncated", s));
        }
        if (u32At(file, at) != kVagMagic) {
            throw FormatError(std::format("sound bank sample {} is not a VAG block", s));
        }
        BankSample sample;
        const u32 layout = u32At(file, at + 4);
        const u32 length = u32At(file, at + 12);
        sample.sampleRate = u32At(file, at + 16);
        for (usize i = 0; i < kVagNameSize && file[at + kVagNameAt + i] != 0; ++i) {
            sample.name.push_back(static_cast<char>(file[at + kVagNameAt + i]));
        }
        at += kVagHeaderSize;
        if (layout != kVagWithDspHeader) {
            throw FormatError(std::format("sound bank sample {} has no DSP-ADPCM header", s));
        }
        if (at + kDspHeaderSize > file.size()) {
            throw FormatError(std::format("sound bank sample {} DSP header is truncated", s));
        }
        sample.sampleCount = u32At(file, at);
        sample.loops = u16At(file, at + 12) != 0;
        sample.loopStart = u32At(file, at + 16);
        sample.loopEnd = u32At(file, at + 20);
        sample.coefficients = readDspAdpcmCoefficients(file.subspan(at + 28, 32));
        at += kDspHeaderSize;
        if (length > file.size() - at) {
            throw FormatError(std::format("sound bank sample {} data is truncated", s));
        }
        sample.adpcm.assign(file.begin() + static_cast<std::ptrdiff_t>(at),
                            file.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        bank.samples.push_back(std::move(sample));
    }
    return bank;
}

std::vector<s16> decodeBankSample(const BankSample& sample) {
    DspAdpcmDecoder decoder(sample.coefficients);
    std::vector<s16> pcm;
    const usize whole = sample.adpcm.size() / DspAdpcmDecoder::kFrameBytes;
    decoder.decode(std::span(sample.adpcm).first(whole * DspAdpcmDecoder::kFrameBytes), pcm);
    if (sample.sampleCount != 0 && pcm.size() > sample.sampleCount) {
        pcm.resize(sample.sampleCount);
    }
    return pcm;
}

} // namespace gdl::formats
