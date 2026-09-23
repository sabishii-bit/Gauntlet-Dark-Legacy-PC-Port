#include "engine/assets/WavFile.h"

#include <string_view>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

constexpr usize kRiffHeaderSize = 12;
constexpr usize kChunkHeaderSize = 8;
constexpr u16 kPcmFormat = 1;
constexpr u16 kSixteenBits = 16;

bool tagIs(std::span<const u8> bytes, usize at, std::string_view tag) {
    for (usize i = 0; i < 4; ++i) {
        if (bytes[at + i] != static_cast<u8>(tag[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

WavData decodeWav(std::span<const u8> bytes) {
    if (bytes.size() < kRiffHeaderSize || !tagIs(bytes, 0, "RIFF") || !tagIs(bytes, 8, "WAVE")) {
        throw FormatError("not a RIFF WAVE file");
    }
    WavData wav;
    bool haveFormat = false;
    bool haveData = false;
    usize at = kRiffHeaderSize;
    while (at + kChunkHeaderSize <= bytes.size()) {
        const u32 size = readU32LE(bytes, at + 4);
        const usize body = at + kChunkHeaderSize;
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
            for (usize i = 0; i < wav.samples.size(); ++i) {
                wav.samples[i] = static_cast<s16>(readU16LE(bytes, body + i * 2));
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
