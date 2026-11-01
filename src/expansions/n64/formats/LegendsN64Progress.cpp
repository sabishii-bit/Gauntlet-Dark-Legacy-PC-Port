#include "expansions/n64/formats/LegendsN64Progress.h"

#include <bit>
#include <cmath>
#include <format>
#include <map>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {
void require(bool value, std::string_view message) {
    if (!value) {
        throw FormatError("N64 Legends progression: " + std::string(message));
    }
}
void range(std::span<const u8> bytes, usize at, usize size) {
    require(at <= bytes.size() && size <= bytes.size() - at, "truncated data");
}
s16 signed16(std::span<const u8> bytes, usize at) {
    return std::bit_cast<s16>(readU16BE(bytes, at));
}
f32 real(std::span<const u8> bytes, usize at) {
    const f32 value = std::bit_cast<f32>(readU32BE(bytes, at));
    require(std::isfinite(value), "non-finite dimensions");
    return value;
}
std::string sequence(std::span<const u8> bytes, usize at) {
    if (readU32BE(bytes, at) == 0) {
        return {};
    }
    std::string result;
    for (usize i = 0; i < 4; ++i) {
        require(bytes[at + i] >= 32 && bytes[at + i] < 127, "invalid sequence name");
        result.push_back(static_cast<char>(bytes[at + i]));
    }
    return result;
}
} // namespace

LegendsN64Obelisks decodeLegendsN64Obelisks(std::span<const u8> placements,
                                            std::span<const u8> traps) {
    range(placements, 0, 92);
    LegendsN64Obelisks result;
    const usize count = readU16BE(placements, 42);
    if (count == 0) {
        return result;
    }
    const usize offset = readU32BE(placements, 12);
    require(offset >= 92 && count <= 16384, "invalid placement table");
    range(placements, offset, count * 24);
    range(traps, 0, 4);
    const usize templateCount = readU32BE(traps, 0);
    require(templateCount <= 4096, "template budget exceeded");
    range(traps, 4, templateCount * 96);
    std::map<u16, usize> templates;
    for (usize i = 0; i < templateCount; ++i) {
        const usize at = 4 + i * 96;
        require(templates.emplace(readU16BE(traps, at), at).second, "duplicate trap template");
    }
    for (usize i = 0; i < count; ++i) {
        const usize at = offset + i * 24;
        const u16 key = readU16BE(placements, at + 8);
        const auto found = templates.find(key);
        require(found != templates.end(), "unknown trap template");
        const usize native = found->second;
        const u16 flags = readU16BE(traps, native + 2);
        if ((flags & 15U) != 9) {
            continue;
        }
        // e00694e4: the placement parameter selects the acquired bit. All
        // seven authored obelisks share template2601; its subtype is not an ID.
        LegendsN64Obelisk item;
        item.nativeIndex = static_cast<u32>(i);
        item.templateKey = key;
        item.flags = flags;
        item.activationFlags = readU32BE(traps, native + 4);
        item.identity = readU32BE(placements, at + 12);
        if (item.identity == 0xadadU || item.identity == 0xadadadadU) {
            item.identity = 0;
        }
        item.placementFlags = placements[at + 10];
        item.minimumPlayers = placements[at + 11];
        item.idleSequence = sequence(traps, native + 12);
        item.spentSequence = sequence(traps, native + 20);
        item.position = Vec3{signed16(placements, at), signed16(placements, at + 2),
                             signed16(placements, at + 4)} /
                        4.0f;
        item.yaw = static_cast<f32>(signed16(placements, at + 6)) / 32;
        item.scale = real(placements, at + 16);
        item.scale = item.scale > .001f ? item.scale : 1;
        item.sound = readU32BE(traps, native + 28);
        item.groundProjected = (flags & 0x20U) == 0;
        item.radius = real(traps, native + 36) * item.scale;
        item.height = real(traps, native + 40) * item.scale;
        require(std::isfinite(item.radius) && std::isfinite(item.height) && item.radius >= 0 &&
                    item.height >= 0,
                "invalid contact dimensions");
        // e0069c90 activates this contact-only trap; e0068118 case9 changes
        // OBRD to OBDN without removing the solid body. Unknown trigger/shape
        // variants remain visible metadata rather than guessed pickups.
        item.supported = key == 0x2601 && flags == 0x1009 && item.activationFlags == 0x02000000 &&
                         item.identity >= 1 && item.identity <= 7 && item.placementFlags == 0 &&
                         item.minimumPlayers <= 4 && readU32BE(traps, native + 32) == 1 &&
                         readU32BE(traps, native + 48) == 5 && readU32BE(traps, native + 72) == 5 &&
                         item.radius > 0 && item.height > 0 && !item.idleSequence.empty() &&
                         !item.spentSequence.empty() && readU32BE(traps, native + 16) == 0;
        if (!item.supported) {
            result.warnings.push_back(std::format(
                "Native obelisk {} uses unsupported identity {}, flags, contact or activation.",
                item.nativeIndex, item.identity));
        }
        result.obelisks.push_back(std::move(item));
    }
    return result;
}

} // namespace gdl::formats
