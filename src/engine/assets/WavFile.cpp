#include "engine/assets/WavFile.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

constexpr std::size_t kRiffHeaderSize = 12;
constexpr std::size_t kChunkHeaderSize = 8;
constexpr std::uint16_t kPcmFormat = 1;
constexpr std::uint16_t kSixteenBits = 16;

bool tagIs(std::span<const std::uint8_t> bytes, std::size_t at, std::string_view tag) {
    for (std::size_t i = 0; i < 4; ++i) {
        if (bytes[at + i] != static_cast<std::uint8_t>(tag[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

WavData decodeWav(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kRiffHeaderSize || !tagIs(bytes, 0, "RIFF") || !tagIs(bytes, 8, "WAVE")) {
        throw FormatError("not a RIFF WAVE file");
    }
    WavData wav;
    bool haveFormat = false;
    bool haveData = false;
    std::size_t at = kRiffHeaderSize;
    while (at + kChunkHeaderSize <= bytes.size()) {
        const std::uint32_t size = readU32LE(bytes, at + 4);
        const std::size_t body = at + kChunkHeaderSize;
        if (size > bytes.size() - body) {
            throw FormatError("WAVE chunk runs past the end of the file");
        }
        if (tagIs(bytes, at, "fmt ")) {
            if (size < 16) {
                throw FormatError("WAVE format chunk is too short");
            }
            if (readU16LE(bytes, body) != kPcmFormat ||
                readU16LE(bytes, body + 14) != kSixteenBits) {
                throw FormatError("only 16-bit PCM WAVE files are supported");
            }
            wav.channels = readU16LE(bytes, body + 2);
            wav.sampleRate = readU32LE(bytes, body + 4);
            haveFormat = true;
        } else if (tagIs(bytes, at, "data")) {
            wav.samples.resize(size / 2);
            for (std::size_t i = 0; i < wav.samples.size(); ++i) {
                wav.samples[i] = static_cast<std::int16_t>(readU16LE(bytes, body + i * 2));
            }
            haveData = true;
        }
        at = body + size + (size % 2);
    }
    if (!haveFormat || !haveData || wav.channels == 0 || wav.sampleRate == 0) {
        throw FormatError("WAVE file lacks a usable format or data chunk");
    }
    return wav;
}

WavData loadWav(const std::filesystem::path& path) {
    return decodeWav(readFile(path));
}

} // namespace gdl
