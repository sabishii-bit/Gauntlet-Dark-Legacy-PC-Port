#include "formats/KeyframeTrack.h"

#include <format>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr u16 kCompressed = 0x2000;  ///< keys after the first are delta-table bytes
constexpr u16 kInitialOnly = 0x4000; ///< a single frame with no key bits
constexpr u16 kKeptFlags = NodeTrack::kChannels | NodeTrack::kPitchYawRoll;

void require(std::span<const u8> file, usize at, usize size, std::string_view what) {
    if (at > file.size() || size > file.size() - at) {
        throw FormatError(std::format("{} lies outside the file", what));
    }
}

f32 readF32(std::span<const u8> file, usize at) {
    return std::bit_cast<f32>(readU32LE(file, at));
}

std::vector<f32> readDeltaTable(std::span<const u8> file, usize at, std::string_view what) {
    require(file, at, KeyHeader::kTableSize * 4, what);
    std::vector<f32> table(KeyHeader::kTableSize);
    for (usize i = 0; i < KeyHeader::kTableSize; ++i) {
        table[i] = readF32(file, at + i * 4);
    }
    return table;
}

/** Frame 0 plus every later frame whose bit is set, one bit per frame, low bit first. */
std::vector<u16> keyedFrames(std::span<const u8> file, usize at, s32 frameCount,
                             std::string_view what) {
    const usize words = (static_cast<usize>(frameCount) + 31) / 32;
    require(file, at, words * 4, what);
    std::vector<u16> frames{0};
    for (s32 f = 1; f < frameCount; ++f) {
        const u32 word = readU32LE(file, at + static_cast<usize>(f / 32) * 4);
        if (((word >> (static_cast<u32>(f) % 32U)) & 1U) != 0) {
            frames.push_back(static_cast<u16>(f));
        }
    }
    return frames;
}

} // namespace

const std::vector<f32>& KeyHeader::tableFor(u32 channel) const {
    if (channel < 3) {
        return angles;
    }
    return channel < 6 ? positions : scales;
}

KeyHeader readKeyHeader(std::span<const u8> file, usize header, std::string_view what) {
    require(file, header, KeyHeader::kSize, what);
    KeyHeader out;
    if (const u32 at = readU32LE(file, header); at != 0) {
        out.angles = readDeltaTable(file, header + at, what);
    }
    if (const u32 at = readU32LE(file, header + 4); at != 0) {
        out.positions = readDeltaTable(file, header + at, what);
    }
    if (const u32 at = readU32LE(file, header + 8); at != 0) {
        out.scales = readDeltaTable(file, header + at, what);
    }
    const std::vector<f32> zeros(KeyHeader::kTableSize, 0.0f);
    for (std::vector<f32>* table : {&out.angles, &out.positions, &out.scales}) {
        if (table->empty()) {
            *table = zeros;
        }
    }
    out.blocks = header + readU32LE(file, header + 12);
    return out;
}

NodeTrack readKeyTrack(std::span<const u8> file, usize info, const KeyHeader& header,
                       s32 frameCount, std::string_view what) {
    require(file, info, KeyHeader::kEntrySize, what);
    NodeTrack track;
    const u16 type = readU16LE(file, info);
    const usize stride = readU16LE(file, info + 2);
    const usize data = header.blocks + readU32LE(file, info + 4);
    track.flags = static_cast<u16>(type & kKeptFlags);
    const u32 channels = track.channelCount();
    if (channels == 0 || frameCount <= 0) {
        track.flags = 0;
        track.frames = {0};
        return track;
    }
    if (channels > stride) {
        throw FormatError(std::format("{} keys more channels than its records hold", what));
    }
    std::vector<u32> present;
    for (u32 c = 0; c < NodeTrack::kChannelCount; ++c) {
        if (track.has(c)) {
            present.push_back(c);
        }
    }

    if ((type & kInitialOnly) != 0) {
        require(file, data, stride * 4, what);
        track.frames = {0};
        for (usize c = 0; c < channels; ++c) {
            track.values.push_back(readF32(file, data + c * 4));
        }
        return track;
    }

    track.frames = keyedFrames(file, data, frameCount, what);
    const usize body = data + ((static_cast<usize>(frameCount) + 31) / 32) * 4;
    if ((type & kCompressed) != 0) {
        require(file, body, stride * 4 + (track.frames.size() - 1) * stride, what);
        for (usize c = 0; c < channels; ++c) {
            track.values.push_back(readF32(file, body + c * 4));
        }
        usize record = body + stride * 4;
        for (usize k = 1; k < track.frames.size(); ++k) {
            for (u32 c = 0; c < channels; ++c) {
                const f32 previous = track.values[(k - 1) * channels + c];
                const f32 step = header.tableFor(present[c])[file[record + c]];
                track.values.push_back(previous + step);
            }
            record += stride;
        }
        return track;
    }

    require(file, body, track.frames.size() * stride * 4, what);
    for (usize k = 0; k < track.frames.size(); ++k) {
        for (u32 c = 0; c < channels; ++c) {
            track.values.push_back(readF32(file, body + (k * stride + c) * 4));
        }
    }
    return track;
}

} // namespace gdl::formats
