#pragma once

#include <bit>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

/**
 * One node's or object's keyframes in one sequence, decoded to plain values: which of the
 * nine channels (rotation, position and scale, x y z each) it keys, the frames that hold keys
 * and the channel values at each. Absent channels stay at 0 (scale at 1). Compressed files
 * store later keys as byte steps through shared delta tables; those are summed back here.
 */
struct NodeTrack {
    static constexpr u16 kRotationX = 0x1;
    static constexpr u16 kRotationY = 0x2;
    static constexpr u16 kRotationZ = 0x4;
    static constexpr u16 kPositionX = 0x10;
    static constexpr u16 kPositionY = 0x20;
    static constexpr u16 kPositionZ = 0x40;
    static constexpr u16 kScaleX = 0x100;
    static constexpr u16 kScaleY = 0x200;
    static constexpr u16 kScaleZ = 0x400;
    static constexpr u16 kChannels = 0x777;
    static constexpr u16 kPitchYawRoll =
        0x8000; ///< rotate pitch, yaw then roll; else roll, yaw, pitch
    static constexpr u32 kChannelCount = 9;

    u32 node = 0;
    u16 flags = 0;           ///< channel bits and the rotation order
    std::vector<u16> frames; ///< keyed frames, ascending, always starting at 0
    std::vector<f32> values; ///< channelCount() values per key, channels in ascending order

    u32 channelCount() const {
        return static_cast<u32>(std::popcount(static_cast<u32>(flags & kChannels)));
    }
    /** The flag bit of channel 0..8. */
    static constexpr u16 channelBit(u32 channel) {
        return static_cast<u16>(1U << (channel + channel / 3));
    }
    bool has(u32 channel) const { return (flags & channelBit(channel)) != 0; }
};

/** A tree's or level's key header: where its key blocks start and the delta tables its
 * compressed keys step through. */
struct KeyHeader {
    static constexpr usize kSize = 28;
    static constexpr usize kEntrySize = 8; ///< one node's entry for one sequence
    static constexpr usize kTableSize = 256;

    usize blocks = 0; ///< absolute offset of the key blocks
    std::vector<f32> angles;
    std::vector<f32> positions;
    std::vector<f32> scales;

    const std::vector<f32>& tableFor(u32 channel) const;
};

/** Reads the key header at `header` (its pointers are relative to it); throws FormatError. */
KeyHeader readKeyHeader(std::span<const u8> file, usize header, std::string_view what);

/** Decodes the keys at `info` (16-bit flags, 16-bit record size, 32-bit offset) for a
 * sequence of `frameCount` frames; an entry keying no channel gives a track of one empty key.
 * Throws FormatError. */
NodeTrack readKeyTrack(std::span<const u8> file, usize info, const KeyHeader& header,
                       s32 frameCount, std::string_view what);

} // namespace gdl::formats
