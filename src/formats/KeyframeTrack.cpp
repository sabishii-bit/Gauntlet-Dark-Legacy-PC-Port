#include "formats/KeyframeTrack.h"

#include <cstddef>
#include <cstdint>
#include <format>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr std::uint16_t kCompressed = 0x2000;  ///< keys after the first are delta-table bytes
constexpr std::uint16_t kInitialOnly = 0x4000; ///< a single frame with no key bits
constexpr std::uint16_t kKeptFlags = NodeTrack::kChannels | NodeTrack::kPitchYawRoll;

void require(std::span<const std::uint8_t> file, std::size_t at, std::size_t size,
             std::string_view what) {
    if (at > file.size() || size > file.size() - at) {
        throw FormatError(std::format("{} lies outside the file", what));
    }
}

float readF32(std::span<const std::uint8_t> file, std::size_t at) {
    return std::bit_cast<float>(readU32LE(file, at));
}

std::vector<float> readDeltaTable(std::span<const std::uint8_t> file, std::size_t at,
                                  std::string_view what) {
    require(file, at, KeyHeader::kTableSize * 4, what);
    std::vector<float> table(KeyHeader::kTableSize);
    for (std::size_t i = 0; i < KeyHeader::kTableSize; ++i) {
        table[i] = readF32(file, at + i * 4);
    }
    return table;
}

/** Frame 0 plus every later frame whose bit is set, one bit per frame, low bit first. */
std::vector<std::uint16_t> keyedFrames(std::span<const std::uint8_t> file, std::size_t at,
                                       std::int32_t frameCount, std::string_view what) {
    const std::size_t words = (static_cast<std::size_t>(frameCount) + 31) / 32;
    require(file, at, words * 4, what);
    std::vector<std::uint16_t> frames{0};
    for (std::int32_t f = 1; f < frameCount; ++f) {
        const std::uint32_t word = readU32LE(file, at + static_cast<std::size_t>(f / 32) * 4);
        if (((word >> (static_cast<std::uint32_t>(f) % 32U)) & 1U) != 0) {
            frames.push_back(static_cast<std::uint16_t>(f));
        }
    }
    return frames;
}

} // namespace

const std::vector<float>& KeyHeader::tableFor(std::uint32_t channel) const {
    if (channel < 3) {
        return angles;
    }
    return channel < 6 ? positions : scales;
}

KeyHeader readKeyHeader(std::span<const std::uint8_t> file, std::size_t header,
                        std::string_view what) {
    require(file, header, KeyHeader::kSize, what);
    KeyHeader out;
    if (const std::uint32_t at = readU32LE(file, header); at != 0) {
        out.angles = readDeltaTable(file, header + at, what);
    }
    if (const std::uint32_t at = readU32LE(file, header + 4); at != 0) {
        out.positions = readDeltaTable(file, header + at, what);
    }
    if (const std::uint32_t at = readU32LE(file, header + 8); at != 0) {
        out.scales = readDeltaTable(file, header + at, what);
    }
    const std::vector<float> zeros(KeyHeader::kTableSize, 0.0f);
    for (std::vector<float>* table : {&out.angles, &out.positions, &out.scales}) {
        if (table->empty()) {
            *table = zeros;
        }
    }
    out.blocks = header + readU32LE(file, header + 12);
    return out;
}

NodeTrack readKeyTrack(std::span<const std::uint8_t> file, std::size_t info,
                       const KeyHeader& header, std::int32_t frameCount, std::string_view what) {
    require(file, info, KeyHeader::kEntrySize, what);
    NodeTrack track;
    const std::uint16_t type = readU16LE(file, info);
    const std::size_t stride = readU16LE(file, info + 2);
    const std::size_t data = header.blocks + readU32LE(file, info + 4);
    track.flags = static_cast<std::uint16_t>(type & kKeptFlags);
    const std::uint32_t channels = track.channelCount();
    if (channels == 0 || frameCount <= 0) {
        track.flags = 0;
        track.frames = {0};
        return track;
    }
    if (channels > stride) {
        throw FormatError(std::format("{} keys more channels than its records hold", what));
    }
    std::vector<std::uint32_t> present;
    for (std::uint32_t c = 0; c < NodeTrack::kChannelCount; ++c) {
        if (track.has(c)) {
            present.push_back(c);
        }
    }

    if ((type & kInitialOnly) != 0) {
        require(file, data, stride * 4, what);
        track.frames = {0};
        for (std::size_t c = 0; c < channels; ++c) {
            track.values.push_back(readF32(file, data + c * 4));
        }
        return track;
    }

    track.frames = keyedFrames(file, data, frameCount, what);
    const std::size_t body = data + ((static_cast<std::size_t>(frameCount) + 31) / 32) * 4;
    if ((type & kCompressed) != 0) {
        require(file, body, stride * 4 + (track.frames.size() - 1) * stride, what);
        for (std::size_t c = 0; c < channels; ++c) {
            track.values.push_back(readF32(file, body + c * 4));
        }
        std::size_t record = body + stride * 4;
        for (std::size_t k = 1; k < track.frames.size(); ++k) {
            for (std::uint32_t c = 0; c < channels; ++c) {
                const float previous = track.values[(k - 1) * channels + c];
                const float step = header.tableFor(present[c])[file[record + c]];
                track.values.push_back(previous + step);
            }
            record += stride;
        }
        return track;
    }

    require(file, body, track.frames.size() * stride * 4, what);
    for (std::size_t k = 0; k < track.frames.size(); ++k) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            track.values.push_back(readF32(file, body + (k * stride + c) * 4));
        }
    }
    return track;
}

} // namespace gdl::formats
