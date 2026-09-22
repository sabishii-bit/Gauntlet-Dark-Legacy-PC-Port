#include "formats/CritterWad.h"

#include <string_view>

#include "engine/core/Error.h"
#include "formats/WadDirectory.h"

namespace gdl::formats {

namespace {

constexpr std::string_view kWhat = "critter wad";
constexpr usize kNameWidth = 16;
constexpr usize kSoundSize = 0x50;
constexpr usize kDamageSize = 0x50;
constexpr usize kDescriptorSize = 0x30;
constexpr usize kNodeSize = 0x50;
constexpr usize kMoveSize = 0x90;
constexpr usize kTypeSize = 0x140;

s16 readS16(std::span<const u8> bytes, usize offset) {
    return static_cast<s16>(readWadU16(bytes, offset, kWhat));
}

s32 readS32(std::span<const u8> bytes, usize offset) {
    return static_cast<s32>(readWadU32(bytes, offset, kWhat));
}

std::array<f32, 3> readVec(std::span<const u8> bytes, usize offset) {
    return {readWadF32(bytes, offset, kWhat), readWadF32(bytes, offset + 4, kWhat),
            readWadF32(bytes, offset + 8, kWhat)};
}

CritterTargetRecord readTarget(std::span<const u8> bytes, usize offset) {
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
std::vector<Record> readRecords(std::span<const u8> bytes, std::span<const WadSection> sections,
                                std::string_view tag, usize size, const Read& read) {
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
    for (u32 i = 0; i < section->count; ++i) {
        out.push_back(read(section->offset + static_cast<usize>(i) * size));
    }
    return out;
}

} // namespace

CritterFile parseCritterWad(std::span<const u8> bytes) {
    const std::vector<WadSection> sections = readWadDirectory(bytes, kWhat);
    CritterFile file;
    file.sounds = readRecords<CritterSoundRecord>(bytes, sections, "SFXX", kSoundSize, [&](usize at) {
        CritterSoundRecord sound;
        sound.name = readWadText(bytes, at + 0x10, kNameWidth, kWhat);
        sound.levelFormat = readWadText(bytes, at + 0x20, kNameWidth, kWhat);
        return sound;
    });
    file.damages = readRecords<CritterDamageRecord>(bytes, sections, "DAMG", kDamageSize, [&](usize at) {
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
        damage.sfxIndex = readS16(bytes, at + 0x40);
        damage.sfx = readS16(bytes, at + 0x42);
        return damage;
    });
    file.descriptors = readRecords<CritterDescriptorRecord>(bytes, sections, "DESC", kDescriptorSize, [&](usize at) {
        CritterDescriptorRecord descriptor;
        descriptor.name = readWadText(bytes, at, kNameWidth, kWhat);
        descriptor.prefix = readWadText(bytes, at + 0x10, kNameWidth, kWhat);
        descriptor.type = readS16(bytes, at + 0x20);
        return descriptor;
    });
    file.nodes = readRecords<CritterNodeRecord>(bytes, sections, "NODE", kNodeSize, [&](usize at) {
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
    file.moves = readRecords<CritterMoveRecord>(bytes, sections, "MOVE", kMoveSize, [&](usize at) {
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
    file.types = readRecords<CritterTypeRecord>(bytes, sections, "TYPE", kTypeSize, [&](usize at) {
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
