#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace gdl::formats {

/**
 * One node's or object's keyframes in one sequence, decoded to plain values: which of the
 * nine channels (rotation, position and scale, x y z each) it keys, the frames that hold keys
 * and the channel values at each. Absent channels stay at 0 (scale at 1). Compressed files
 * store later keys as byte steps through shared delta tables; those are summed back here.
 */
struct NodeTrack {
    static constexpr std::uint16_t kRotationX = 0x1;
    static constexpr std::uint16_t kRotationY = 0x2;
    static constexpr std::uint16_t kRotationZ = 0x4;
    static constexpr std::uint16_t kPositionX = 0x10;
    static constexpr std::uint16_t kPositionY = 0x20;
    static constexpr std::uint16_t kPositionZ = 0x40;
    static constexpr std::uint16_t kScaleX = 0x100;
    static constexpr std::uint16_t kScaleY = 0x200;
    static constexpr std::uint16_t kScaleZ = 0x400;
    static constexpr std::uint16_t kChannels = 0x777;
    static constexpr std::uint16_t kPitchYawRoll =
        0x8000; ///< rotate pitch, yaw then roll; else roll, yaw, pitch
    static constexpr std::uint32_t kChannelCount = 9;

    std::uint32_t node = 0;
    std::uint16_t flags = 0;           ///< channel bits and the rotation order
    std::vector<std::uint16_t> frames; ///< keyed frames, ascending, always starting at 0
    std::vector<float> values; ///< channelCount() values per key, channels in ascending order

    std::uint32_t channelCount() const {
        return static_cast<std::uint32_t>(
            std::popcount(static_cast<std::uint32_t>(flags & kChannels)));
    }
    /** The flag bit of channel 0..8. */
    static constexpr std::uint16_t channelBit(std::uint32_t channel) {
        return static_cast<std::uint16_t>(1U << (channel + channel / 3));
    }
    bool has(std::uint32_t channel) const { return (flags & channelBit(channel)) != 0; }
};

/** A tree's or level's key header: where its key blocks start and the delta tables its
 * compressed keys step through. */
struct KeyHeader {
    static constexpr std::size_t kSize = 28;
    static constexpr std::size_t kEntrySize = 8; ///< one node's entry for one sequence
    static constexpr std::size_t kTableSize = 256;

    std::size_t blocks = 0; ///< absolute offset of the key blocks
    std::vector<float> angles;
    std::vector<float> positions;
    std::vector<float> scales;

    const std::vector<float>& tableFor(std::uint32_t channel) const;
};

/** Reads the key header at `header` (its pointers are relative to it); throws FormatError. */
KeyHeader readKeyHeader(std::span<const std::uint8_t> file, std::size_t header,
                        std::string_view what);

/** Decodes the keys at `info` (16-bit flags, 16-bit record size, 32-bit offset) for a
 * sequence of `frameCount` frames; an entry keying no channel gives a track of one empty key.
 * Throws FormatError. */
NodeTrack readKeyTrack(std::span<const std::uint8_t> file, std::size_t info,
                       const KeyHeader& header, std::int32_t frameCount, std::string_view what);

} // namespace gdl::formats
