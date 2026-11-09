#include "expansions/dreamcast/formats/LegendsDreamcastCritter.h"

#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <string_view>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {
void require(bool condition, std::string_view message) {
    if (!condition)
        throw FormatError("Dreamcast critter: " + std::string(message));
}
void range(std::span<const u8> bytes, usize at, usize size) {
    require(at <= bytes.size() && size <= bytes.size() - at, "truncated record");
}
s16 small(std::span<const u8> bytes, usize at) {
    return std::bit_cast<s16>(readU16LE(bytes, at));
}
f32 real(std::span<const u8> bytes, usize at) {
    const auto result = std::bit_cast<f32>(readU32LE(bytes, at));
    require(std::isfinite(result), "non-finite value");
    return result;
}
std::string text(std::span<const u8> bytes, usize at, usize width) {
    range(bytes, at, width);
    std::string result;
    for (usize i = 0; i < width && bytes[at + i] != 0; ++i) {
        require(bytes[at + i] >= 32 && bytes[at + i] < 127, "invalid name");
        result.push_back(static_cast<char>(bytes[at + i]));
    }
    return result;
}
std::array<f32, 3> vector(std::span<const u8> bytes, usize at) {
    return {real(bytes, at), real(bytes, at + 4), real(bytes, at + 8)};
}
CritterTargetRecord target(std::span<const u8> bytes, usize at) {
    return {real(bytes, at),      real(bytes, at + 4),  real(bytes, at + 8), real(bytes, at + 12),
            real(bytes, at + 16), real(bytes, at + 20), real(bytes, at + 24)};
}
struct Section {
    usize offset;
    usize count;
};
class Wad {
public:
    explicit Wad(std::span<const u8> bytes) : m_bytes(bytes) {
        range(bytes, 0, 8);
        m_end = readU32LE(bytes, 0);
        const usize count = readU32LE(bytes, 4);
        require(m_end >= 8 && count <= 64, "invalid directory");
        range(bytes, m_end, count * 12);
        for (usize i = 0; i < count; ++i) {
            const usize at = m_end + i * 12;
            const u32 code = readU32LE(bytes, at);
            std::string tag;
            for (u32 shift : {24U, 16U, 8U, 0U})
                tag.push_back(static_cast<char>((code >> shift) & 255U));
            require(
                m_sections.emplace(tag, Section{readU32LE(bytes, at + 4), readU32LE(bytes, at + 8)})
                    .second,
                "duplicate section");
        }
    }
    template <class T, class Reader>
    std::vector<T> records(std::string_view name, usize stride, Reader read) const {
        const auto found = m_sections.find(name);
        require(found != m_sections.end(), "missing section");
        const auto [at, count] = found->second;
        require(count <= 4096, "record budget exceeded");
        if (count == 0)
            return {};
        usize end = m_end;
        for (const auto& [tag, section] : m_sections) {
            require(tag == name || section.count == 0 || section.offset != at,
                    "aliased nonempty sections");
            if (section.offset > at && section.offset < end)
                end = section.offset;
        }
        require(at >= 8 && at <= end && count <= (end - at) / stride,
                "overlapping or truncated section");
        range(m_bytes, at, count * stride);
        std::vector<T> result;
        result.reserve(count);
        for (usize i = 0; i < count; ++i)
            result.push_back(read(at + i * stride));
        return result;
    }

private:
    std::span<const u8> m_bytes;
    usize m_end = 0;
    std::map<std::string, Section, std::less<>> m_sections;
};

void validate(const CritterFile& file) {
    require(!file.types.empty() && !file.descriptors.empty() && !file.moves.empty(),
            "empty combat definition");
    const auto reference = [](s32 index, usize size) {
        require(index == -1 || (index >= 0 && static_cast<usize>(index) < size),
                "invalid record reference");
    };
    const auto slice = [](s16 first, s16 count, usize size) {
        require(count >= 0 &&
                    (count == 0 || (first >= 0 && static_cast<usize>(first) <= size &&
                                    static_cast<usize>(count) <= size - static_cast<usize>(first))),
                "invalid type range");
    };
    for (const auto& type : file.types) {
        require(type.descriptorIndex >= 0, "missing descriptor");
        reference(type.descriptorIndex, file.descriptors.size());
        reference(type.childIndex, file.types.size());
        reference(type.parentIndex, file.types.size());
        reference(type.hitSoundFar, file.sounds.size());
        reference(type.hitSoundClose, file.sounds.size());
        slice(type.moveIndex, type.moveCount, file.moves.size());
        slice(type.patternIndex, type.patternCount, file.patterns.size());
        slice(type.colBase, type.colCount, file.nodes.size());
        require(type.moveCount > 0 && type.maxHealth > 0 && type.radius >= 0 &&
                    type.wallRadius >= 0,
                "invalid body dimensions or health");
        for (s32 i = 0; i < type.moveCount; ++i)
            reference(file.moves[static_cast<usize>(type.moveIndex + i)].link,
                      static_cast<usize>(type.moveCount));
        for (s32 i = 0; i < type.patternCount; ++i) {
            for (const s16 move : file.patterns[static_cast<usize>(type.patternIndex + i)].moves) {
                if (move < 0)
                    break;
                reference(move, static_cast<usize>(type.moveCount));
            }
        }
    }
    for (const auto& move : file.moves) {
        reference(move.damage0, file.damages.size());
        reference(move.damage1, file.damages.size());
        reference(move.sfx, file.sounds.size());
        reference(move.sfx2, file.sounds.size());
    }
    for (const auto& node : file.nodes)
        reference(node.sfxIndex, file.damages.size());
    for (const auto& damage : file.damages) {
        reference(damage.sfxIndex, file.sounds.size());
        reference(damage.sfx, file.sounds.size());
        reference(damage.morph, file.sounds.size());
        reference(damage.morphEnd, file.sounds.size());
    }
    for (usize i = 0; i < file.sounds.size(); ++i) {
        reference(file.sounds[i].link, file.sounds.size());
        std::set<s32> visited;
        for (s32 link = static_cast<s32>(i); link >= 0;
             link = file.sounds[static_cast<usize>(link)].link) {
            reference(link, file.sounds.size());
            require(visited.insert(link).second, "cyclic effect chain");
        }
    }
    for (usize i = 0; i < file.types.size(); ++i) {
        std::set<s32> visited;
        for (s32 child = static_cast<s32>(i); child >= 0;
             child = file.types[static_cast<usize>(child)].childIndex)
            require(visited.insert(child).second, "cyclic child chain");
    }
}
} // namespace

CritterFile decodeLegendsDreamcastCritter(std::span<const u8> bytes) {
    const Wad wad(bytes);
    CritterFile out;
    // 1ST_READ.BIN 8c085c40 / 8c086020 fix up these Dreamcast-specific
    // strides. Neither the N64 nor GameCube raw structures are interchangeable.
    out.descriptors = wad.records<CritterDescriptorRecord>("DESC", 0x24, [&](usize at) {
        return CritterDescriptorRecord{text(bytes, at, 16), text(bytes, at + 0x10, 8),
                                       small(bytes, at + 0x18)};
    });
    out.moves = wad.records<CritterMoveRecord>("MOVE", 0x80, [&](usize at) {
        CritterMoveRecord move;
        move.type = std::bit_cast<s32>(readU32LE(bytes, at));
        // 8c0895e0 dispatches 85 as a continuous contact window (CHARGE),
        // and 87 snapshots the target through 8c0bbe00. GDL inserted SPRAY.
        if (move.type >= 0x85 && move.type <= 0x87)
            ++move.type;
        move.flags = readU16LE(bytes, at + 4);
        move.priority = small(bytes, at + 6);
        move.name = text(bytes, at + 8, 16);
        move.anim = text(bytes, at + 0x18, 16);
        move.colnode = text(bytes, at + 0x28, 16);
        move.target = target(bytes, at + 0x38);
        move.frameStart = small(bytes, at + 0x58);
        move.frameStart2 = small(bytes, at + 0x5a);
        move.damage0 = small(bytes, at + 0x5c);
        move.damage1 = small(bytes, at + 0x5e);
        move.frameEnd = small(bytes, at + 0x60);
        move.frameEnd2 = small(bytes, at + 0x62);
        move.link = small(bytes, at + 0x64);
        move.interrupt = small(bytes, at + 0x66);
        move.cooldown = real(bytes, at + 0x68);
        move.speed = real(bytes, at + 0x6c);
        move.turnRate = real(bytes, at + 0x70);
        move.sfx = small(bytes, at + 0x74);
        move.sfxFrame = small(bytes, at + 0x76);
        move.sfx2 = small(bytes, at + 0x78);
        move.sfx2Frame = small(bytes, at + 0x7a);
        move.hold = real(bytes, at + 0x7c);
        return move;
    });
    out.patterns = wad.records<CritterPatternRecord>("PTRN", 0x44, [&](usize at) {
        CritterPatternRecord pattern;
        pattern.flags = readU16LE(bytes, at);
        for (usize i = 0; i < pattern.moves.size(); ++i)
            pattern.moves[i] = small(bytes, at + 0x14 + i * 2);
        pattern.cooldown = real(bytes, at + 0x24);
        pattern.target = target(bytes, at + 0x28);
        return pattern;
    });
    out.damages = wad.records<CritterDamageRecord>("DAMG", 0x48, [&](usize at) {
        CritterDamageRecord damage;
        damage.type = small(bytes, at);
        damage.behaviorFlags = small(bytes, at + 2);
        damage.flags = readU32LE(bytes, at + 4);
        damage.radius = real(bytes, at + 8);
        damage.maxDistance = real(bytes, at + 0xc);
        damage.minDistance = real(bytes, at + 0x10);
        damage.yaw = real(bytes, at + 0x14);
        damage.minDot = real(bytes, at + 0x18);
        damage.pitch = real(bytes, at + 0x1c);
        damage.offset = vector(bytes, at + 0x20);
        damage.damage = real(bytes, at + 0x2c);
        damage.minSpeed = real(bytes, at + 0x30);
        damage.maxSpeed = real(bytes, at + 0x34);
        damage.gravity = real(bytes, at + 0x38);
        // 8c085f00 resolves all four indices through the SFXX table.
        damage.sfxIndex = small(bytes, at + 0x3c);
        damage.sfx = small(bytes, at + 0x3e);
        damage.morph = small(bytes, at + 0x40);
        damage.morphEnd = small(bytes, at + 0x42);
        // 8c08875e passes this lifetime (or15 when nonpositive) to morph setup.
        damage.morphLife = real(bytes, at + 0x44);
        return damage;
    });
    out.sounds = wad.records<CritterSoundRecord>("SFXX", 0x48, [&](usize at) {
        CritterSoundRecord sound;
        sound.flags = readU32LE(bytes, at);
        sound.link = std::bit_cast<s32>(readU32LE(bytes, at + 4));
        sound.name = text(bytes, at + 0x10, 16);
        sound.levelFormat = text(bytes, at + 0x20, 16);
        // 8c085e20 passes these two shorts to the native visual initializer.
        sound.custom0 = small(bytes, at + 0x30);
        sound.custom1 = small(bytes, at + 0x32);
        sound.offset = vector(bytes, at + 0x34);
        sound.life = real(bytes, at + 0x40);
        sound.rate = real(bytes, at + 0x44);
        return sound;
    });
    out.nodes = wad.records<CritterNodeRecord>("NODE", 0x48, [&](usize at) {
        CritterNodeRecord node;
        node.nodeName = text(bytes, at, 16);
        node.position = vector(bytes, at + 0x14);
        node.radius = real(bytes, at + 0x20);
        node.maxTargetDistance = real(bytes, at + 0x24);
        node.targetScoreScale = real(bytes, at + 0x28);
        node.attach = text(bytes, at + 0x2c, 16);
        node.damageScale = real(bytes, at + 0x3c);
        node.healthScale = real(bytes, at + 0x40);
        node.flags = small(bytes, at + 0x44);
        node.sfxIndex = small(bytes, at + 0x46);
        return node;
    });
    out.types = wad.records<CritterTypeRecord>("TYPE", 0x118, [&](usize at) {
        CritterTypeRecord type;
        type.descriptorIndex = small(bytes, at);
        type.subtype = small(bytes, at + 2);
        type.suffix = text(bytes, at + 4, 8);
        type.rootNode = text(bytes, at + 0xc, 16);
        type.lookNode0 = text(bytes, at + 0x1c, 16);
        type.lookNode1 = text(bytes, at + 0x2c, 16);
        type.noSkinNode = text(bytes, at + 0x3c, 16);
        type.lookYawRate0 = real(bytes, at + 0x54);
        type.lookYawRate1 = real(bytes, at + 0x58);
        type.lookPitchRate0 = real(bytes, at + 0x5c);
        type.lookPitchRate1 = real(bytes, at + 0x60);
        type.lookPitchBias0 = real(bytes, at + 0x64);
        type.lookPitchBias1 = real(bytes, at + 0x68);
        type.target = target(bytes, at + 0x6c);
        type.typeFlags = readU32LE(bytes, at + 0x88);
        type.radius = real(bytes, at + 0x8c);
        type.wallRadius = real(bytes, at + 0x90);
        type.defaultPos = vector(bytes, at + 0x94);
        type.roamRadius = real(bytes, at + 0xa0);
        type.floorOffset = real(bytes, at + 0xa4);
        // CritterGeo publishes the target centre as root+(0, TYPE.AC, 0)
        // (8c087994..7a8), consumed by CalcTarget8c08c928. B4..BC is not
        // GameCube's XYZ origin: treating it as one displaces melee by 25+
        // units on Dragon/Chimera. Rotate8c08a3ce reads its limit from B0.
        (void)real(bytes, at + 0xa8); // separate camera attention height
        type.originOffset = {0, real(bytes, at + 0xac), 0};
        type.turnLimit = real(bytes, at + 0xb0);
        (void)vector(bytes, at + 0xb4);
        (void)real(bytes, at + 0xc0);
        type.damageScale = real(bytes, at + 0xc4);
        type.armor = real(bytes, at + 0xc8);
        type.maxHealth = real(bytes, at + 0xcc);
        type.expValue = real(bytes, at + 0xd0);
        type.wakeThreshold = real(bytes, at + 0xd4);
        type.pressureReferenceDamage = real(bytes, at + 0xd8);
        type.hitSoundFar = small(bytes, at + 0xdc);
        type.hitSoundClose = small(bytes, at + 0xde);
        type.meterPieces = small(bytes, at + 0xe4);
        type.meterAdvance = small(bytes, at + 0xe6);
        type.meterLeftInset = small(bytes, at + 0xe8);
        type.meterRightInset = small(bytes, at + 0xea);
        type.moveCount = small(bytes, at + 0xec);
        type.moveIndex = small(bytes, at + 0xee);
        type.patternCount = small(bytes, at + 0xf0);
        type.patternIndex = small(bytes, at + 0xf2);
        type.colCount = small(bytes, at + 0xf4);
        type.colBase = small(bytes, at + 0xf6);
        type.childIndex = small(bytes, at + 0xf8);
        type.parentIndex = small(bytes, at + 0xfa);
        return type;
    });
    // No shipped Dreamcast arena uses ADDA. Do not reinterpret the metadata-only
    // PBOSS attachment as an N64 or GameCube record until its native use is supported.
    const auto attachments = wad.records<u8>("ADDA", 0x28, [](usize) { return u8{0}; });
    require(attachments.empty(), "additional-tree records are not supported");
    validate(out);
    return out;
}
} // namespace gdl::formats
