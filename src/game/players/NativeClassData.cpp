#include "game/players/NativeClassData.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "engine/core/Error.h"
#include "engine/math/Math.h"

#include "formats/PlayerDataWad.h"
#include "formats/WadDirectory.h"
#include "game/players/ClassData.h"

namespace gdl::game {
namespace {

constexpr std::string_view kWhat = "native class data";
constexpr usize kHeaderSize = 16;
constexpr usize kClassRecordSize = 0x180;
constexpr usize kEffectSize = 0x50;
constexpr usize kStrikeSize = 0x58;

/** Runtime tables must be complete; the inspection decoder also permits partial WADs. */
void validateTables(std::span<const u8> bytes) {
    const auto sections = formats::readWadDirectory(bytes, kWhat);
    const usize directory = formats::readWadU32(bytes, 0, kWhat);
    const auto requireSection = [&](std::string_view tag, usize count, usize stride) {
        const auto* section = formats::findWadSection(sections, tag);
        if (section == nullptr) {
            if (count != 0) {
                throw FormatError("native class data: missing table");
            }
            return;
        }
        if (std::ranges::count(sections, tag, &formats::WadSection::tag) != 1 ||
            section->count != count || section->offset < kHeaderSize) {
            throw FormatError("native class data: invalid table count or offset");
        }
        usize end = bytes.size();
        if (directory >= section->offset) {
            end = directory;
        }
        for (const auto& other : sections) {
            if (&other != section && other.offset >= section->offset) {
                end = std::min(end, usize{other.offset});
            }
        }
        if (count > (end - section->offset) / stride) {
            throw FormatError("native class data: truncated table");
        }
    };
    requireSection("PDAT", 1, kClassRecordSize);
    const auto* record = formats::findWadSection(sections, "PDAT");
    requireSection("SFXX", formats::readWadU16(bytes, record->offset, kWhat), kEffectSize);
    requireSection("DAMG", formats::readWadU16(bytes, record->offset + 2, kWhat), kStrikeSize);
}

Vec3 vectorOf(const std::array<f32, 3>& value) {
    return {value[0], value[1], value[2]};
}

} // namespace

ClassStats parseNativeClassStats(std::span<const u8> bytes) {
    validateTables(bytes);
    const auto source = formats::parsePlayerDataWad(bytes);
    ClassStats stats;
    stats.fightMin = source.fightMin;
    stats.fightMax = source.fightMax;
    stats.speedMin = source.speedMin;
    stats.speedMax = source.speedMax;
    stats.armorMin = source.armorMin;
    stats.armorMax = source.armorMax;
    stats.magicMin = source.magicMin;
    stats.magicMax = source.magicMax;
    stats.height = source.height;
    stats.width = source.width;
    stats.collisionY = source.collisionY;
    stats.weaponOffset = vectorOf(source.weaponOffset);
    stats.familiarOffset = vectorOf(source.familiarOffset);
    stats.familiarShotOffset = vectorOf(source.familiarShotOffset);
    static_assert(ClassStats::kGlowTiers == formats::PlayerClassRecord::kGlowTiers);
    for (usize tier = 0; tier < stats.weaponGlowOffsets.size(); ++tier) {
        stats.weaponGlowOffsets[tier] = vectorOf(source.weaponGlowOffsets[tier]);
        stats.weaponGlowScales[tier] = vectorOf(source.weaponGlowScales[tier]);
    }
    stats.powerupTime = source.powerupTime;
    stats.streakForward = source.streakForward;
    stats.moves = {source.moves[0], source.moves[1], source.moves[2], source.moves[3],
                   source.moves[4], source.moves[5], source.moves[6], source.moves[7],
                   source.moves[8], source.moves[10]};
    for (const auto& from : source.effects) {
        stats.moveEffects.push_back({from.next, from.tree, from.sound, vectorOf(from.offset),
                                     from.scale, from.flags, from.lifetime, from.radius,
                                     from.alphaMod});
    }
    for (const auto& from : source.strikes) {
        MoveStrike strike;
        strike.type = from.type;
        strike.hitRadius = from.hitRadius;
        strike.radius = from.radius;
        strike.delay = from.delay;
        strike.maxTime = from.maxTime;
        strike.arc = from.arc;
        strike.offset = vectorOf(from.offset);
        strike.amount = from.amount;
        strike.speed = from.speedMin + 0.5f * (from.speedMax - from.speedMin);
        strike.angle = from.angle;
        strike.damageType = from.damageType;
        strike.effect = from.effect;
        strike.hitEffect = from.hitEffect;
        strike.loopEffect = from.loopEffect;
        strike.next = from.next;
        strike.startFrame = from.startFrame;
        strike.endFrame = from.endFrame;
        strike.flags = from.flags;
        strike.help = from.help;
        stats.moveStrikes.push_back(strike);
    }
    return stats;
}

} // namespace gdl::game
