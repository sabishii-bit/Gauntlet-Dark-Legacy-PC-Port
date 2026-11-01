#include "expansions/n64/formats/LegendsN64Switches.h"

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
        throw FormatError("N64 Legends switches: " + std::string(message));
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
std::string targetName(std::span<const u8> strings, usize at) {
    require(at < strings.size(), "target outside string pool");
    std::string result;
    for (usize i = at; i < strings.size(); ++i) {
        const u8 value = strings[i];
        if (value == 0) {
            require(!result.empty(), "empty world target");
            return result;
        }
        require(value >= 32 && value < 127 && result.size() < 255, "invalid world target name");
        result.push_back(static_cast<char>(value));
    }
    throw FormatError("N64 Legends switches: unterminated world target");
}
} // namespace

LegendsN64Switches decodeLegendsN64Switches(std::span<const u8> placements,
                                            std::span<const u8> traps) {
    range(placements, 0, 92);
    LegendsN64Switches result;
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
        if ((flags & 15U) != 2) {
            continue;
        }
        // e0062dfc copies the source header+32 pool into runtime+0x38;
        // e00694e4 resolves this relative offset through e006dea0 by WOBJ name.
        const usize names = readU32BE(placements, 32);
        const usize nameBytes = readU16BE(placements, 54);
        require(names >= 92, "invalid string pool");
        range(placements, names, nameBytes);
        u32 nameOffset = readU32BE(placements, at + 12);
        if (nameOffset == 0xadadU || nameOffset == 0xadadadadU) {
            nameOffset = 0; // Native uninitialized-value normalization.
        }
        LegendsN64Switch item;
        item.nativeIndex = static_cast<u32>(i);
        item.templateKey = key;
        item.flags = flags;
        item.activationFlags = readU32BE(traps, native + 4);
        item.placementFlags = placements[at + 10];
        item.minimumPlayers = placements[at + 11];
        item.target = targetName(placements.subspan(names, nameBytes), nameOffset);
        item.idleSequence = sequence(traps, native + 12);
        item.activeSequence = sequence(traps, native + 16);
        item.position = Vec3{signed16(placements, at), signed16(placements, at + 2),
                             signed16(placements, at + 4)} /
                        4.0f;
        item.yaw = static_cast<f32>(signed16(placements, at + 6)) / 32;
        item.scale = real(placements, at + 16);
        item.scale = item.scale > .001f ? item.scale : 1;
        item.firstEndpoint = signed16(placements, at + 20);
        item.secondEndpoint = signed16(placements, at + 22);
        item.sound = readU32BE(traps, native + 28);
        item.groundProjected = (flags & 0x20U) == 0;
        const u32 shape = readU32BE(traps, native + 32);
        // Invisible trip plates multiply the native collision dimensions by4,
        // after binding their hidden tree (e00694e4). They still activate.
        const f32 collisionScale = item.scale * (item.hidden() ? 4.0f : 1.0f);
        item.radius = real(traps, native + 36) * collisionScale;
        item.height = real(traps, native + 40) * collisionScale;
        require(std::isfinite(item.radius) && std::isfinite(item.height) && item.radius >= 0 &&
                    item.height >= 0,
                "invalid contact dimensions");
        const bool motion = item.mode() == 0x100 || item.mode() == 0x200 || item.mode() == 0x500 ||
                            item.mode() == 0x800 || item.mode() == 0x900 || item.mode() == 0xa00;
        item.supported = motion && shape == 1 && item.radius > 0 && item.height > 0 &&
                         (item.activationFlags & 0x07000000U) == 0x02000000U;
        if (!item.supported) {
            result.warnings.push_back(std::format(
                "Native switch {} targeting {} uses unsupported mode 0x{:03x}, contact {}, or "
                "activation flags 0x{:08x}.",
                item.nativeIndex, item.target, item.mode(), shape, item.activationFlags));
        }
        result.switches.push_back(std::move(item));
    }
    return result;
}

} // namespace gdl::formats
