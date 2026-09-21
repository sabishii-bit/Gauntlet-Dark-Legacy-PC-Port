#include "formats/PlayerDataWad.h"

#include <string_view>
#include <vector>

#include "engine/core/Error.h"
#include "formats/WadDirectory.h"

namespace gdl::formats {

namespace {

constexpr std::string_view kWhat = "player data wad";
constexpr usize kMovesOffset = 0x0C;
constexpr usize kEffectSize = 0x50;
constexpr usize kStrikeSize = 0x58;
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
    for (usize axis = 0; axis < record.weaponOffset.size(); ++axis) {
        record.weaponOffset[axis] = readWadF32(bytes, body + 20 + axis * 4, kWhat);
    }
    const auto s16At = [&](usize at) { return static_cast<s16>(readWadU16(bytes, at, kWhat)); };
    for (usize move = 0; move < record.moves.size(); ++move) {
        record.moves[move] = s16At(offset + kMovesOffset + move * 2);
    }
    if (const WadSection* effects = findWadSection(sections, "SFXX"); effects != nullptr) {
        for (usize i = 0; i < record.effectCount; ++i) {
            const usize at = effects->offset + i * kEffectSize;
            MoveEffectRecord effect;
            effect.flags = readWadU32(bytes, at, kWhat);
            effect.next = static_cast<s32>(readWadU32(bytes, at + 4, kWhat));
            effect.tree = readWadText(bytes, at + 0x10, 16, kWhat);
            effect.sound = readWadText(bytes, at + 0x20, 16, kWhat);
            for (usize axis = 0; axis < 3; ++axis) {
                effect.offset[axis] = readWadF32(bytes, at + 0x34 + axis * 4, kWhat);
            }
            effect.scale = readWadF32(bytes, at + 0x48, kWhat);
            record.effects.push_back(std::move(effect));
        }
    }
    if (const WadSection* strikes = findWadSection(sections, "DAMG"); strikes != nullptr) {
        for (usize i = 0; i < record.damageCount; ++i) {
            const usize at = strikes->offset + i * kStrikeSize;
            MoveStrikeRecord strike;
            strike.type = s16At(at);
            strike.flags = s16At(at + 2);
            strike.damageType = readWadU32(bytes, at + 4, kWhat);
            strike.hitRadius = readWadF32(bytes, at + 0x08, kWhat);
            strike.radius = readWadF32(bytes, at + 0x0C, kWhat);
            strike.delay = readWadF32(bytes, at + 0x14, kWhat);
            strike.minTime = readWadF32(bytes, at + 0x18, kWhat);
            strike.maxTime = readWadF32(bytes, at + 0x1C, kWhat);
            strike.arc = readWadF32(bytes, at + 0x24, kWhat);
            for (usize axis = 0; axis < 3; ++axis) {
                strike.offset[axis] = readWadF32(bytes, at + 0x2C + axis * 4, kWhat);
            }
            strike.amount = readWadF32(bytes, at + 0x38, kWhat);
            strike.speedMin = readWadF32(bytes, at + 0x3C, kWhat);
            strike.speedMax = readWadF32(bytes, at + 0x40, kWhat);
            strike.effect = s16At(at + 0x48);
            strike.hitEffect = s16At(at + 0x4A);
            strike.loopEffect = s16At(at + 0x4C);
            strike.next = s16At(at + 0x4E);
            strike.startFrame = s16At(at + 0x50);
            strike.endFrame = s16At(at + 0x52);
            strike.help = s16At(at + 0x54);
            record.strikes.push_back(strike);
        }
    }
    return record;
}

} // namespace gdl::formats
