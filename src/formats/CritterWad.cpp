#include "formats/CritterWad.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/core/Error.h"

#include "formats/WadDirectory.h"

namespace gdl::formats {

namespace {

constexpr std::string_view kWhat = "critter wad";
constexpr std::size_t kNameWidth = 16;
constexpr std::size_t kSoundSize = 0x50;
constexpr std::size_t kDamageSize = 0x50;
constexpr std::size_t kDescriptorSize = 0x30;
constexpr std::size_t kNodeSize = 0x50;
constexpr std::size_t kMoveSize = 0x90;
constexpr std::size_t kTypeSize = 0x140;

std::int16_t readS16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::int16_t>(readWadU16(bytes, offset, kWhat));
}

std::int32_t readS32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::int32_t>(readWadU32(bytes, offset, kWhat));
}

std::array<float, 3> readVec(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return {readWadF32(bytes, offset, kWhat), readWadF32(bytes, offset + 4, kWhat),
            readWadF32(bytes, offset + 8, kWhat)};
}

CritterTargetRecord readTarget(std::span<const std::uint8_t> bytes, std::size_t offset) {
    CritterTargetRecord target;
    target.minDistance = readWadF32(bytes, offset, kWhat);
    target.maxDistance = readWadF32(bytes, offset + 0x04, kWhat);
    target.yaw = readWadF32(bytes, offset + 0x08, kWhat);
    target.minDot = readWadF32(bytes, offset + 0x0C, kWhat);
    target.minRateScale = readWadF32(bytes, offset + 0x10, kWhat);
    target.maxRateScale = readWadF32(bytes, offset + 0x14, kWhat);
    target.idleGate = readWadF32(bytes, offset + 0x18, kWhat);
    target.maxVertical = readWadF32(bytes, offset + 0x1C, kWhat);
    return target;
}

/** The records of a section, `size` bytes each, read one at a time. */
template <typename Record, typename Read>
std::vector<Record> readRecords(std::span<const std::uint8_t> bytes,
                                std::span<const WadSection> sections, std::string_view tag,
                                std::size_t size, const Read& read) {
    std::vector<Record> out;
    const WadSection* section = findWadSection(sections, tag);
    if (section == nullptr) {
        return out;
    }
    if (bytes.size() < section->offset ||
        (bytes.size() - section->offset) / size < section->count) {
        throw FormatError("critter wad: section out of bounds");
    }
    out.reserve(section->count);
    for (std::uint32_t i = 0; i < section->count; ++i) {
        out.push_back(read(section->offset + static_cast<std::size_t>(i) * size));
    }
    return out;
}

} // namespace

CritterFile parseCritterWad(std::span<const std::uint8_t> bytes) {
    const std::vector<WadSection> sections = readWadDirectory(bytes, kWhat);
    CritterFile file;
    file.sounds =
        readRecords<CritterSoundRecord>(bytes, sections, "SFXX", kSoundSize, [&](std::size_t at) {
            CritterSoundRecord sound;
            sound.flags = readWadU32(bytes, at, kWhat);
            sound.link = readS32(bytes, at + 0x04);
            sound.name = readWadText(bytes, at + 0x10, kNameWidth, kWhat);
            sound.levelFormat = readWadText(bytes, at + 0x20, kNameWidth, kWhat);
            sound.offset = readVec(bytes, at + 0x30);
            sound.life = readWadF32(bytes, at + 0x3C, kWhat);
            sound.rate = readWadF32(bytes, at + 0x40, kWhat);
            sound.custom0 = readS16(bytes, at + 0x44);
            sound.custom1 = readS16(bytes, at + 0x46);
            sound.tint = readWadU32(bytes, at + 0x48, kWhat);
            sound.scale = readWadF32(bytes, at + 0x4C, kWhat);
            return sound;
        });
    file.damages =
        readRecords<CritterDamageRecord>(bytes, sections, "DAMG", kDamageSize, [&](std::size_t at) {
            CritterDamageRecord damage;
            damage.type = readS16(bytes, at);
            damage.behaviorFlags = readS16(bytes, at + 0x02);
            damage.flags = readWadU32(bytes, at + 0x04, kWhat);
            damage.radius = readWadF32(bytes, at + 0x08, kWhat);
            damage.maxDistance = readWadF32(bytes, at + 0x0C, kWhat);
            damage.minDistance = readWadF32(bytes, at + 0x10, kWhat);
            damage.yaw = readWadF32(bytes, at + 0x14, kWhat);
            damage.minDot = readWadF32(bytes, at + 0x18, kWhat);
            damage.pitch = readWadF32(bytes, at + 0x1C, kWhat);
            damage.offset = readVec(bytes, at + 0x20);
            damage.damage = readWadF32(bytes, at + 0x2C, kWhat);
            damage.minSpeed = readWadF32(bytes, at + 0x30, kWhat);
            damage.maxSpeed = readWadF32(bytes, at + 0x34, kWhat);
            damage.gravity = readWadF32(bytes, at + 0x38, kWhat);
            damage.sfxIndex = readS16(bytes, at + 0x40);
            damage.sfx = readS16(bytes, at + 0x42);
            return damage;
        });
    file.descriptors = readRecords<CritterDescriptorRecord>(
        bytes, sections, "DESC", kDescriptorSize, [&](std::size_t at) {
            CritterDescriptorRecord descriptor;
            descriptor.name = readWadText(bytes, at, kNameWidth, kWhat);
            descriptor.prefix = readWadText(bytes, at + 0x10, kNameWidth, kWhat);
            descriptor.type = readS16(bytes, at + 0x20);
            return descriptor;
        });
    file.nodes =
        readRecords<CritterNodeRecord>(bytes, sections, "NODE", kNodeSize, [&](std::size_t at) {
            CritterNodeRecord node;
            node.nodeName = readWadText(bytes, at, kNameWidth, kWhat);
            node.flags = readS16(bytes, at + 0x10);
            node.sfxIndex = readS16(bytes, at + 0x12);
            node.maxTargetDistance = readWadF32(bytes, at + 0x18, kWhat);
            node.targetScoreScale = readWadF32(bytes, at + 0x1C, kWhat);
            node.position = readVec(bytes, at + 0x20);
            node.radius = readWadF32(bytes, at + 0x2C, kWhat);
            node.attach = readWadText(bytes, at + 0x30, kNameWidth, kWhat);
            node.damageScale = readWadF32(bytes, at + 0x40, kWhat);
            node.healthScale = readWadF32(bytes, at + 0x44, kWhat);
            return node;
        });
    file.moves =
        readRecords<CritterMoveRecord>(bytes, sections, "MOVE", kMoveSize, [&](std::size_t at) {
            CritterMoveRecord move;
            move.type = readS32(bytes, at);
            move.flags = readWadU32(bytes, at + 0x04, kWhat);
            move.priority = readS32(bytes, at + 0x08);
            move.name = readWadText(bytes, at + 0x10, kNameWidth, kWhat);
            move.anim = readWadText(bytes, at + 0x20, kNameWidth, kWhat);
            move.colnode = readWadText(bytes, at + 0x30, kNameWidth, kWhat);
            move.frameStart = readS32(bytes, at + 0x40);
            move.frameStart2 = readS32(bytes, at + 0x44);
            move.damage0 = readS16(bytes, at + 0x48);
            move.damage1 = readS16(bytes, at + 0x4A);
            move.framePeriod = readWadF32(bytes, at + 0x4C, kWhat);
            move.frameEnd = readS16(bytes, at + 0x50);
            move.frameEnd2 = readS16(bytes, at + 0x52);
            move.link = readS16(bytes, at + 0x54);
            move.interrupt = readS16(bytes, at + 0x56);
            move.sfx = readS16(bytes, at + 0x58);
            move.sfxFrame = readS16(bytes, at + 0x5A);
            move.sfx2 = readS16(bytes, at + 0x5C);
            move.sfx2Frame = readS16(bytes, at + 0x5E);
            move.target = readTarget(bytes, at + 0x60);
            move.cooldown = readWadF32(bytes, at + 0x80, kWhat);
            move.speed = readWadF32(bytes, at + 0x84, kWhat);
            move.turnRate = readWadF32(bytes, at + 0x88, kWhat);
            move.hold = readWadF32(bytes, at + 0x8C, kWhat);
            return move;
        });
    file.types =
        readRecords<CritterTypeRecord>(bytes, sections, "TYPE", kTypeSize, [&](std::size_t at) {
            CritterTypeRecord type;
            type.suffix = readWadText(bytes, at, kNameWidth, kWhat);
            type.rootNode = readWadText(bytes, at + 0x10, kNameWidth, kWhat);
            type.descriptorIndex = readS16(bytes, at + 0x50);
            type.subtype = readS16(bytes, at + 0x52);
            type.typeFlags = readWadU32(bytes, at + 0x5C, kWhat);
            type.radius = readWadF32(bytes, at + 0x78, kWhat);
            type.wallRadius = readWadF32(bytes, at + 0x7C, kWhat);
            type.target = readTarget(bytes, at + 0x80);
            type.defaultPos = readVec(bytes, at + 0xA0);
            type.speed = readWadF32(bytes, at + 0xAC, kWhat);
            type.floorOffset = readWadF32(bytes, at + 0xB0, kWhat);
            type.vertDrift = readWadF32(bytes, at + 0xB4, kWhat);
            type.damageScale = readWadF32(bytes, at + 0xB8, kWhat);
            type.armor = readWadF32(bytes, at + 0xBC, kWhat);
            type.originOffset = readVec(bytes, at + 0xC0);
            type.turnLimit = readWadF32(bytes, at + 0xCC, kWhat);
            type.shieldFlags = readWadU32(bytes, at + 0xE0, kWhat);
            type.maxHealth = readWadF32(bytes, at + 0xE4, kWhat);
            type.expValue = readWadF32(bytes, at + 0xE8, kWhat);
            type.wakeThreshold = readWadF32(bytes, at + 0xEC, kWhat);
            type.healthBarOffset = readVec(bytes, at + 0xD0);
            type.hitSoundClose = readS16(bytes, at + 0xF4);
            type.hitSoundFar = readS16(bytes, at + 0xF6);
            type.meterPieces = readS16(bytes, at + 0xF8);
            type.meterAdvance = readS16(bytes, at + 0xFA);
            type.meterLeftInset = readS16(bytes, at + 0xFC);
            type.meterRightInset = readS16(bytes, at + 0xFE);
            type.moveCount = readS16(bytes, at + 0x110);
            type.moveIndex = readS16(bytes, at + 0x112);
            type.patternCount = readS16(bytes, at + 0x114);
            type.patternIndex = readS16(bytes, at + 0x116);
            type.colCount = readS16(bytes, at + 0x118);
            type.colBase = readS16(bytes, at + 0x11A);
            type.childIndex = readS16(bytes, at + 0x11C);
            type.parentIndex = readS16(bytes, at + 0x11E);
            return type;
        });
    return file;
}

} // namespace gdl::formats
