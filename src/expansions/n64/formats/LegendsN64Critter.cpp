#include "expansions/n64/formats/LegendsN64Critter.h"

#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <string_view>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {
void require(bool condition, std::string_view detail) {
    if (!condition) {
        throw FormatError("N64 Legends critter: " + std::string(detail));
    }
}
void range(std::span<const u8> bytes, usize at, usize size) {
    require(at <= bytes.size() && size <= bytes.size() - at, "truncated record");
}
s16 small(std::span<const u8> bytes, usize at) {
    return std::bit_cast<s16>(readU16BE(bytes, at));
}
f32 real(std::span<const u8> bytes, usize at) {
    const auto result = std::bit_cast<f32>(readU32BE(bytes, at));
    require(std::isfinite(result), "non-finite field");
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
    // Boss.bin e08869ec consumes six floats, not GDL's eight-field target.
    return {real(bytes, at),      real(bytes, at + 4),  real(bytes, at + 8),
            real(bytes, at + 12), real(bytes, at + 16), real(bytes, at + 20)};
}
struct Section {
    usize offset;
    usize count;
};
class Wad {
public:
    explicit Wad(std::span<const u8> bytes) : m_bytes(bytes) {
        range(bytes, 0, 8);
        const usize directory = readU32BE(bytes, 0), count = readU32BE(bytes, 4);
        require(count <= 64 && directory >= 8, "invalid directory");
        range(bytes, directory, count * 12);
        m_end = directory;
        for (usize i = 0; i < count; ++i) {
            const usize at = directory + i * 12;
            const auto name = text(bytes, at, 4);
            require(m_sections
                        .emplace(name, Section{readU32BE(bytes, at + 4), readU32BE(bytes, at + 8)})
                        .second,
                    "duplicate section");
        }
    }
    template <class T, class Reader>
    std::vector<T> records(std::string_view name, usize stride, Reader read) const {
        const auto it = m_sections.find(name);
        require(it != m_sections.end(), "missing section");
        const auto [at, count] = it->second;
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
                "section overlap or truncation");
        range(m_bytes, at, count * stride);
        std::vector<T> result;
        result.reserve(count);
        for (usize i = 0; i < count; ++i)
            result.push_back(read(at + i * stride));
        return result;
    }
    Section section(std::string_view name) const {
        const auto found = m_sections.find(name);
        require(found != m_sections.end(), "missing section");
        return found->second;
    }

private:
    std::span<const u8> m_bytes;
    usize m_end = 0;
    std::map<std::string, Section, std::less<>> m_sections;
};
} // namespace

std::vector<LegendsN64BoneName> decodeLegendsN64BoneNames(std::span<const u8> bytes) {
    const Wad wad(bytes);
    const auto data = wad.section("BDAT");
    range(bytes, data.offset, data.count);
    return wad.records<LegendsN64BoneName>("BNAM", 8, [&](usize at) {
        const usize name = readU32BE(bytes, at);
        require(name >= data.offset && name < data.offset + data.count, "bone name outside BDAT");
        usize length = 0;
        while (length < 64 && name + length < data.offset + data.count &&
               bytes[name + length] != 0) {
            ++length;
        }
        require(length != 0 && length < 64 && name + length < data.offset + data.count,
                "invalid bone name");
        const u32 node = readU32BE(bytes, at + 4);
        require(node < 4096, "bone index exceeds hierarchy budget");
        return LegendsN64BoneName{text(bytes, name, length), node};
    });
}

std::optional<u32> findLegendsN64Bone(std::span<const LegendsN64BoneName> names,
                                      std::string_view wanted) {
    if (wanted.empty()) {
        return {};
    }
    // Boss.bin e0880438 passes width16 to game.bin e002e664, which subtracts
    // one and scans BNAM in authored order using case-insensitive e00105e4.
    const auto key = toLowerAscii(wanted.substr(0, 15));
    for (const auto& entry : names) {
        if (toLowerAscii(std::string_view{entry.name}.substr(0, 15)) == key) {
            return entry.node;
        }
    }
    return {};
}

CritterFile decodeLegendsN64Critter(std::span<const u8> bytes) {
    const Wad wad(bytes);
    CritterFile out;
    // Native Boss.bin e08804b0/e08806a4 fix up these exact record strides.
    out.descriptors = wad.records<CritterDescriptorRecord>("DESC", 0x24, [&](usize at) {
        return CritterDescriptorRecord{text(bytes, at, 16), text(bytes, at + 0x10, 8),
                                       small(bytes, at + 0x18)};
    });
    out.moves = wad.records<CritterMoveRecord>("MOVE", 0x78, [&](usize at) {
        CritterMoveRecord move;
        move.type = std::bit_cast<s32>(readU32BE(bytes, at));
        // Native target-area snapshot is MOVE0x87; the shared GDL runtime uses0x88.
        if (move.type == 0x87)
            move.type = 0x88;
        move.flags = readU16BE(bytes, at + 4);
        move.priority = small(bytes, at + 6);
        move.name = text(bytes, at + 8, 16);
        move.anim = text(bytes, at + 0x18, 4);
        move.colnode = text(bytes, at + 0x28, 16);
        move.target = target(bytes, at + 0x38);
        move.frameStart = small(bytes, at + 0x54);
        move.frameStart2 = small(bytes, at + 0x56);
        move.damage0 = small(bytes, at + 0x58);
        move.damage1 = small(bytes, at + 0x5a);
        move.frameEnd = small(bytes, at + 0x5c);
        move.frameEnd2 = small(bytes, at + 0x5e);
        move.link = small(bytes, at + 0x60);
        move.interrupt = small(bytes, at + 0x62);
        move.cooldown = real(bytes, at + 0x64);
        move.speed = real(bytes, at + 0x68);
        move.turnRate = real(bytes, at + 0x6c);
        // e08842e8 holds only the completed death and special 0x86 reaction.
        // No per-MOVE hold field exists in this native layout.
        move.hold = move.type == 17 ? 2.0f : move.type == 0x86 ? 1.5f : 0.0f;
        move.sfx = small(bytes, at + 0x70);
        move.sfxFrame = small(bytes, at + 0x72);
        move.sfx2 = small(bytes, at + 0x74);
        move.sfx2Frame = small(bytes, at + 0x76);
        return move;
    });
    out.patterns = wad.records<CritterPatternRecord>("PTRN", 0x40, [&](usize at) {
        CritterPatternRecord pattern;
        pattern.flags = readU16BE(bytes, at);
        for (usize i = 0; i < 8; ++i)
            pattern.moves[i] = small(bytes, at + 0x14 + i * 2);
        pattern.cooldown = real(bytes, at + 0x24);
        pattern.target = target(bytes, at + 0x28);
        return pattern;
    });
    out.damages = wad.records<CritterDamageRecord>("DAMG", 0x44, [&](usize at) {
        CritterDamageRecord damage;
        damage.type = small(bytes, at);
        damage.behaviorFlags = small(bytes, at + 2);
        // Authored DMG semantics, not the runtime PTrap hit-category word:
        // e0886ed0 maps 1/2/3/4 to fire/electric/light/acid and 0x20/40/80
        // to knockdown/blown-away/stun. Retain the shared GDL defense/item
        // behavior for higher authoring bits (gas, piercing, reflection).
        damage.flags = readU32BE(bytes, at + 4);
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
        damage.sfxIndex = small(bytes, at + 0x3c);
        damage.sfx = small(bytes, at + 0x3e);
        damage.morph = small(bytes, at + 0x40);
        return damage;
    });
    out.sounds = wad.records<CritterSoundRecord>("SFXX", 0x44, [&](usize at) {
        CritterSoundRecord sound;
        sound.flags = readU32BE(bytes, at);
        sound.link = small(bytes, at + 4);
        sound.name = text(bytes, at + 0xc, 16);
        sound.levelFormat = text(bytes, at + 0x1c, 16);
        sound.offset = vector(bytes, at + 0x30);
        // e0881b84/e0881a28 ignore native 0x10/0x20. Their GDL meanings
        // (floor projection/arena callback) must not be imported accidentally.
        sound.flags &= ~0x30U;
        // Native e0881a20 (skin command) is an empty function. No GameCube skin
        // animation, particle emitter or lifetime is fabricated from unused words.
        if ((sound.flags & 0x100U) != 0) {
            sound.name.clear();
            sound.flags &= ~0x100U;
        }
        return sound;
    });
    out.nodes = wad.records<CritterNodeRecord>("NODE", 0x2c, [&](usize at) {
        CritterNodeRecord node;
        node.nodeName = text(bytes, at, 16);
        node.flags = small(bytes, at + 0x12);
        node.position = vector(bytes, at + 0x14);
        node.radius = real(bytes, at + 0x20);
        // e0885328 uses these for assisted aim only, not damage multiplication.
        node.maxTargetDistance = real(bytes, at + 0x24);
        node.targetScoreScale = real(bytes, at + 0x28);
        return node;
    });
    out.types = wad.records<CritterTypeRecord>("TYPE", 0xf8, [&](usize at) {
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
        type.typeFlags = readU32BE(bytes, at + 0x84);
        type.radius = real(bytes, at + 0x88);
        type.wallRadius = real(bytes, at + 0x8c);
        type.roamRadius = real(bytes, at + 0x90);
        type.floorOffset = real(bytes, at + 0x94);
        type.originOffset = {0, real(bytes, at + 0x9c), 0};
        type.turnLimit = real(bytes, at + 0xa0);
        type.maxHealth = real(bytes, at + 0xac);
        type.expValue = real(bytes, at + 0xb0);
        type.wakeThreshold = real(bytes, at + 0xb4);
        type.pressureReferenceDamage = real(bytes, at + 0xb8);
        type.hitSoundFar = small(bytes, at + 0xbc);
        type.hitSoundClose = small(bytes, at + 0xbe);
        type.meterPieces = small(bytes, at + 0xc4);
        type.meterAdvance = small(bytes, at + 0xc6);
        type.meterLeftInset = small(bytes, at + 0xc8);
        type.meterRightInset = small(bytes, at + 0xca);
        type.moveCount = small(bytes, at + 0xcc);
        type.moveIndex = small(bytes, at + 0xce);
        type.patternCount = small(bytes, at + 0xd0);
        type.patternIndex = small(bytes, at + 0xd2);
        type.colCount = small(bytes, at + 0xd4);
        type.colBase = small(bytes, at + 0xd6);
        type.childIndex = small(bytes, at + 0xd8);
        type.parentIndex = small(bytes, at + 0xda);
        type.defaultPos = {0, 999, 0}; // Native bosses use the authored spawn as their home.
        return type;
    });
    out.attachments = wad.records<CritterAttachmentRecord>("ADDA", 0x18, [&](usize at) {
        CritterAttachmentRecord item;
        // e0880bdc creates a separate world-root at the actor position, adding
        // only ADDA+0xc to Y. The trailing word is its runtime linked-list cache.
        const auto owner = readU32BE(bytes, at);
        require(owner <= 32767, "invalid additional-tree owner");
        item.typeIndex = static_cast<s16>(owner);
        item.flags = 0; // Common bit 1 means body-following; native ADDA stays world-rooted.
        item.tree = text(bytes, at + 4, 8);
        item.offset = {0, real(bytes, at + 0xc), 0};
        return item;
    });
    require(!out.descriptors.empty() && !out.types.empty() && !out.moves.empty(),
            "empty boss data");
    const auto reference = [](s32 index, usize count) {
        require(index == -1 || (index >= 0 && static_cast<usize>(index) < count),
                "invalid record reference");
    };
    const auto slice = [](s16 first, s16 count, usize size) {
        require(count >= 0 &&
                    (count == 0 || (first >= 0 && static_cast<usize>(first) <= size &&
                                    static_cast<usize>(count) <= size - static_cast<usize>(first))),
                "invalid type record range");
    };
    for (const auto& type : out.types) {
        require(type.descriptorIndex >= 0, "missing descriptor");
        reference(type.descriptorIndex, out.descriptors.size());
        reference(type.childIndex, out.types.size());
        reference(type.parentIndex, out.types.size());
        reference(type.hitSoundFar, out.sounds.size());
        reference(type.hitSoundClose, out.sounds.size());
        slice(type.moveIndex, type.moveCount, out.moves.size());
        slice(type.patternIndex, type.patternCount, out.patterns.size());
        slice(type.colBase, type.colCount, out.nodes.size());
        require(type.moveCount > 0 && type.maxHealth > 0 && type.radius >= 0 &&
                    type.wallRadius >= 0,
                "invalid body dimensions or health");
        for (s32 i = 0; i < type.moveCount; ++i) {
            const auto& move = out.moves[static_cast<usize>(type.moveIndex + i)];
            reference(move.link, static_cast<usize>(type.moveCount));
        }
        for (s32 i = 0; i < type.patternCount; ++i) {
            for (const auto move : out.patterns[static_cast<usize>(type.patternIndex + i)].moves) {
                if (move < 0)
                    break;
                reference(move, static_cast<usize>(type.moveCount));
            }
        }
    }
    for (const auto& move : out.moves) {
        reference(move.damage0, out.damages.size());
        reference(move.damage1, out.damages.size());
        reference(move.sfx, out.sounds.size());
        reference(move.sfx2, out.sounds.size());
    }
    for (const auto& attachment : out.attachments)
        reference(attachment.typeIndex, out.types.size());
    for (const auto& damage : out.damages) {
        reference(damage.sfxIndex, out.sounds.size());
        reference(damage.sfx, out.sounds.size());
        reference(damage.morph, out.sounds.size());
    }
    for (usize i = 0; i < out.sounds.size(); ++i) {
        std::set<s32> visited;
        for (s32 link = static_cast<s32>(i); link >= 0;
             link = out.sounds[static_cast<usize>(link)].link) {
            reference(link, out.sounds.size());
            require(visited.insert(link).second, "cyclic sound chain");
        }
    }
    return out;
}
} // namespace gdl::formats
