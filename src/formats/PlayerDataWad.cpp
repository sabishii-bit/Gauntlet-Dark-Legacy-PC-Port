#include "formats/PlayerDataWad.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "engine/core/Error.h"

namespace gdl::formats {

namespace {

constexpr usize kHeaderSize = 16;
constexpr usize kDirectoryEntrySize = 16;
constexpr usize kRecordSize = 0x180;
constexpr usize kRangesOffset = 0x28;
constexpr usize kBodyOffset = 0x48;
constexpr std::array<u8, 4> kRecordTag{'T', 'A', 'D', 'P'}; ///< "PDAT" stored reversed

u32 readU32(std::span<const u8> bytes, usize offset) {
    if (offset + 4 > bytes.size()) {
        throw FormatError("player data wad: truncated");
    }
    u32 value = 0;
    std::memcpy(&value, bytes.data() + offset, 4);
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    return value;
}

u16 readU16(std::span<const u8> bytes, usize offset) {
    if (offset + 2 > bytes.size()) {
        throw FormatError("player data wad: truncated");
    }
    u16 value = 0;
    std::memcpy(&value, bytes.data() + offset, 2);
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    return value;
}

f32 readF32(std::span<const u8> bytes, usize offset) {
    return std::bit_cast<f32>(readU32(bytes, offset));
}

} // namespace

PlayerClassRecord parsePlayerDataWad(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderSize) {
        throw FormatError("player data wad: too small");
    }
    const u32 directoryOffset = readU32(bytes, 0);
    const u32 sectionCount = readU32(bytes, 4);
    if (directoryOffset > bytes.size() ||
        sectionCount > (bytes.size() - directoryOffset) / kDirectoryEntrySize) {
        throw FormatError("player data wad: bad directory");
    }
    for (u32 i = 0; i < sectionCount; ++i) {
        const usize entry = directoryOffset + usize{i} * kDirectoryEntrySize;
        const auto tag = bytes.subspan(entry, kRecordTag.size());
        if (!std::equal(kRecordTag.begin(), kRecordTag.end(), tag.begin())) {
            continue;
        }
        const u32 offset = readU32(bytes, entry + 4);
        if (offset > bytes.size() || bytes.size() - offset < kRecordSize) {
            throw FormatError("player data wad: record out of bounds");
        }
        PlayerClassRecord record;
        record.effectCount = readU16(bytes, offset);
        record.damageCount = readU16(bytes, offset + 2);
        const usize ranges = offset + kRangesOffset;
        record.fightMin = readF32(bytes, ranges);
        record.fightMax = readF32(bytes, ranges + 4);
        record.speedMin = readF32(bytes, ranges + 8);
        record.speedMax = readF32(bytes, ranges + 12);
        record.armorMin = readF32(bytes, ranges + 16);
        record.armorMax = readF32(bytes, ranges + 20);
        record.magicMin = readF32(bytes, ranges + 24);
        record.magicMax = readF32(bytes, ranges + 28);
        const usize body = offset + kBodyOffset;
        record.height = readF32(bytes, body);
        record.width = readF32(bytes, body + 4);
        record.attachY = readF32(bytes, body + 8);
        record.collisionY = readF32(bytes, body + 12);
        record.powerupTime = readF32(bytes, body + 16);
        return record;
    }
    throw FormatError("player data wad: no PDAT section");
}

} // namespace gdl::formats
