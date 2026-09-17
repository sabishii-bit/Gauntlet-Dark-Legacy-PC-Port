#include "formats/PlayerDataWad.h"

#include <string_view>
#include <vector>

#include "engine/core/Error.h"
#include "formats/WadDirectory.h"

namespace gdl::formats {

namespace {

constexpr std::string_view kWhat = "player data wad";
constexpr usize kRecordSize = 0x180;
constexpr usize kRangesOffset = 0x28;
constexpr usize kBodyOffset = 0x48;

} // namespace

PlayerClassRecord parsePlayerDataWad(std::span<const u8> bytes) {
    const std::vector<WadSection> sections = readWadDirectory(bytes, kWhat);
    const WadSection* section = findWadSection(sections, "PDAT");
    if (section == nullptr) {
        throw FormatError("player data wad: no PDAT section");
    }
    const usize offset = section->offset;
    if (bytes.size() - offset < kRecordSize) {
        throw FormatError("player data wad: record out of bounds");
    }
    PlayerClassRecord record;
    record.effectCount = readWadU16(bytes, offset, kWhat);
    record.damageCount = readWadU16(bytes, offset + 2, kWhat);
    const usize ranges = offset + kRangesOffset;
    record.fightMin = readWadF32(bytes, ranges, kWhat);
    record.fightMax = readWadF32(bytes, ranges + 4, kWhat);
    record.speedMin = readWadF32(bytes, ranges + 8, kWhat);
    record.speedMax = readWadF32(bytes, ranges + 12, kWhat);
    record.armorMin = readWadF32(bytes, ranges + 16, kWhat);
    record.armorMax = readWadF32(bytes, ranges + 20, kWhat);
    record.magicMin = readWadF32(bytes, ranges + 24, kWhat);
    record.magicMax = readWadF32(bytes, ranges + 28, kWhat);
    const usize body = offset + kBodyOffset;
    record.height = readWadF32(bytes, body, kWhat);
    record.width = readWadF32(bytes, body + 4, kWhat);
    record.attachY = readWadF32(bytes, body + 8, kWhat);
    record.collisionY = readWadF32(bytes, body + 12, kWhat);
    record.powerupTime = readWadF32(bytes, body + 16, kWhat);
    return record;
}

} // namespace gdl::formats
