#include "formats/SoundBank.h"

#include <cstddef>
#include <cstdint>
#include <format>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr std::size_t kHeaderSize = 20;
constexpr std::uint32_t kMagic = 0x4B4E4256; // "KNBV"
constexpr std::size_t kParameterWords = 3;
constexpr std::uint16_t kLastStep = 0x8000;
constexpr std::uint16_t kLoopStartStep = 0x4000;
constexpr std::uint16_t kLoopBackStep = 0x2000;
constexpr std::uint16_t kSampleMask = 0x0FFF;
constexpr std::size_t kVagHeaderSize = 48;
constexpr std::uint32_t kVagMagic = 0x56414770; // "VAGp"
constexpr std::uint32_t kVagWithDspHeader = 0x28;
constexpr std::size_t kDspHeaderSize = 96;
constexpr std::size_t kVagNameAt = 32;
constexpr std::size_t kVagNameSize = 16;

std::uint32_t u32At(std::span<const std::uint8_t> file, std::size_t at) {
    return readU32BE(file, at);
}

std::uint16_t u16At(std::span<const std::uint8_t> file, std::size_t at) {
    return readU16BE(file, at);
}

} // namespace

SoundBank SoundBank::parse(std::span<const std::uint8_t> file) {
    if (file.size() < kHeaderSize) {
        throw FormatError("sound bank is too small for its header");
    }
    if (u32At(file, 0) != kMagic) {
        throw FormatError("sound bank has no KNBV signature");
    }
    const std::uint32_t callBytes = u32At(file, 4);
    const std::uint32_t callCount = u32At(file, 12);
    const std::uint32_t sampleCount = u32At(file, 16);
    if (callBytes % 2 != 0 || callBytes > file.size() - kHeaderSize) {
        throw FormatError("sound bank call table lies outside the file");
    }

    SoundBank bank;
    const std::size_t callWords = callBytes / 2;
    std::size_t word = 0;
    for (std::uint32_t c = 0; c < callCount; ++c) {
        SoundCall call;
        bool ended = false;
        while (word < callWords && !ended) {
            const std::uint16_t value = u16At(file, kHeaderSize + word * 2);
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

    std::size_t at = kHeaderSize + callBytes;
    for (std::uint32_t s = 0; s < sampleCount; ++s) {
        if (at + kVagHeaderSize > file.size()) {
            throw FormatError(std::format("sound bank sample {} header is truncated", s));
        }
        if (u32At(file, at) != kVagMagic) {
            throw FormatError(std::format("sound bank sample {} is not a VAG block", s));
        }
        BankSample sample;
        const std::uint32_t layout = u32At(file, at + 4);
        const std::uint32_t length = u32At(file, at + 12);
        sample.sampleRate = u32At(file, at + 16);
        for (std::size_t i = 0; i < kVagNameSize && file[at + kVagNameAt + i] != 0; ++i) {
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

std::vector<std::int16_t> decodeBankSample(const BankSample& sample) {
    DspAdpcmDecoder decoder(sample.coefficients);
    std::vector<std::int16_t> pcm;
    const std::size_t whole = sample.adpcm.size() / DspAdpcmDecoder::kFrameBytes;
    decoder.decode(std::span(sample.adpcm).first(whole * DspAdpcmDecoder::kFrameBytes), pcm);
    if (sample.sampleCount != 0 && pcm.size() > sample.sampleCount) {
        pcm.resize(sample.sampleCount);
    }
    return pcm;
}

} // namespace gdl::formats
