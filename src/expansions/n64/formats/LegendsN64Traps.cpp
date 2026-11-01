#include "expansions/n64/formats/LegendsN64Traps.h"

#include <bit>
#include <cmath>
#include <format>
#include <map>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {
constexpr u32 kProgramBase = 0xe0000400U;
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw FormatError("N64 Legends traps: " + std::string(message));
    }
}
void range(std::span<const u8> data, usize at, usize length) {
    require(at <= data.size() && length <= data.size() - at, "truncated native data");
}
s16 integer(std::span<const u8> data, usize at) {
    return std::bit_cast<s16>(readU16BE(data, at));
}
f32 real(std::span<const u8> data, usize at) {
    const auto value = std::bit_cast<f32>(readU32BE(data, at));
    require(std::isfinite(value), "non-finite value");
    return value;
}
std::string sequence(std::span<const u8> data, usize at) {
    std::string result;
    for (usize i = 0; i < 4; ++i) {
        require(data[at + i] >= 32 && data[at + i] < 127, "invalid sequence name");
        result.push_back(static_cast<char>(data[at + i]));
    }
    return result;
}
std::string nativeName(std::span<const u8> program, u32 pointer) {
    require(pointer >= kProgramBase, "invalid map name pointer");
    const usize at = pointer - kProgramBase;
    std::string result;
    for (usize i = 0; i <= 32; ++i) {
        range(program, at + i, 1);
        const auto c = program[at + i];
        if (c == 0) {
            require(!result.empty(), "empty map name");
            return result;
        }
        require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_',
                "unsafe map name");
        result.push_back(static_cast<char>(c));
    }
    throw FormatError("N64 Legends traps: unterminated map name");
}
} // namespace

LegendsN64Traps decodeLegendsN64Traps(std::span<const u8> placements,
                                      std::span<const u8> templates) {
    range(placements, 0, 92);
    LegendsN64Traps result;
    const usize count = readU16BE(placements, 42);
    if (count == 0) {
        return result;
    }
    const usize offset = readU32BE(placements, 12);
    require(offset >= 92 && count <= 16384, "invalid placement table");
    range(placements, offset, count * 24);
    range(templates, 0, 4);
    const usize templatesCount = readU32BE(templates, 0);
    require(templatesCount <= 4096, "template budget exceeded");
    range(templates, 4, templatesCount * 96);
    std::map<u16, usize> lookup;
    for (usize i = 0; i < templatesCount; ++i) {
        const usize at = 4 + i * 96;
        require(lookup.emplace(readU16BE(templates, at), at).second, "duplicate template");
    }
    for (usize i = 0; i < count; ++i) {
        const usize at = offset + i * 24;
        const u16 key = readU16BE(placements, at + 8);
        const auto found = lookup.find(key);
        require(found != lookup.end(), "unknown template");
        const usize native = found->second;
        const u16 flags = readU16BE(templates, native + 2);
        const u16 kind = flags & 15U;
        if (kind != 1 && kind != 10) {
            continue;
        }
        const u32 shape = readU32BE(templates, native + 32);
        if ((flags & 0x2000U) == 0 || readU32BE(templates, native + 4) != 0x05000000U ||
            (kind == 10 ? shape != 5 : (shape != 1 && shape != 3))) {
            result.warnings.push_back(
                std::format("Native trap {} has unsupported activation or collision.", i));
            continue;
        }
        LegendsN64Trap trap;
        trap.nativeIndex = static_cast<u32>(i);
        trap.templateKey = key;
        trap.flags = flags;
        trap.placementFlags = placements[at + 10];
        trap.minimumPlayers = placements[at + 11];
        trap.position = Vec3{integer(placements, at), integer(placements, at + 2),
                             integer(placements, at + 4)} /
                        4.0f;
        trap.yaw = static_cast<f32>(integer(placements, at + 6)) / 32;
        trap.scale = real(placements, at + 16);
        trap.scale = trap.scale > .001f ? trap.scale : 1;
        require(trap.scale <= 64, "invalid trap scale");
        trap.groundProjected = (flags & 0x20U) == 0;
        trap.shape = shape;
        // e0068118 creates a separate type2 attack for limb traps. Its radius
        // is the raw template value, unaffected by drawable/hidden-item scale.
        const f32 collisionScale =
            kind == 10 ? 1 : trap.scale * ((trap.placementFlags & 8U) != 0 ? 4.0f : 1.0f);
        trap.dimensions = Vec3{real(templates, native + 36), real(templates, native + 40),
                               real(templates, native + 44)} *
                          collisionScale;
        require(trap.dimensions.x > 0 &&
                    (kind == 10 ? trap.dimensions.y == 0 : trap.dimensions.y > 0) &&
                    trap.dimensions.z >= 0 && (shape != 3 || trap.dimensions.z > 0) &&
                    trap.dimensions.x < 1024 && trap.dimensions.y < 1024 &&
                    trap.dimensions.z < 1024,
                "invalid trap dimensions");
        if (kind == 10) {
            trap.contactNode = "HITNODE"; // e00ab4ac, searched recursively by e0027210.
        }
        trap.restFrames = readU16BE(templates, native + 8);
        trap.damage = templates[native + 10];
        trap.activeFrames = templates[native + 11];
        trap.idleSequence = sequence(templates, native + 12);
        trap.activeSequence = sequence(templates, native + 16);
        trap.sound = readU32BE(templates, native + 28);
        require(trap.idleSequence != trap.activeSequence && trap.damage > 0,
                "invalid cycling trap");
        result.traps.push_back(std::move(trap));
    }
    return result;
}

std::optional<f32> legendsN64TrapScale(std::span<const u8> program, std::string_view levelName) {
    if (program.empty()) {
        return std::nullopt;
    }
    std::map<u32, std::string> stems;
    bool terminated = false;
    for (usize at = 0xe006c360U - kProgramBase; at < 0xe006c468U - kProgramBase; at += 24) {
        range(program, at, 24);
        const u32 realm = readU32BE(program, at);
        if (realm == 16) {
            terminated = true;
            break;
        }
        require(realm < 16 &&
                    stems.emplace(realm, nativeName(program, readU32BE(program, at + 8))).second,
                "invalid realm table");
    }
    require(terminated, "unterminated realm table");
    std::optional<std::pair<u32, u32>> chosen;
    terminated = false;
    for (usize i = 0; i < 128; ++i) {
        const usize at = 0xe006c468U - kProgramBase + i * 40;
        range(program, at, 40);
        const u32 realm = readU32BE(program, at);
        const u32 level = readU32BE(program, at + 4);
        if (realm == 16) {
            terminated = true;
            break;
        }
        const auto stem = stems.find(realm);
        require(stem != stems.end() && level <= 10, "invalid level table");
        if (toLowerAscii(stem->second + nativeName(program, readU32BE(program, at + 12))) ==
            toLowerAscii(levelName)) {
            chosen = {realm, level};
        }
    }
    require(terminated, "unterminated level table");
    if (!chosen) {
        return std::nullopt;
    }
    // e004161c uses the third metadata byte as the difficulty-table index.
    for (usize i = 0; i < 64; ++i) {
        const usize at = 0xe0045ca8U - kProgramBase + i * 5;
        range(program, at, 5);
        if (program[at] == 0xff) {
            return std::nullopt;
        }
        if (program[at] == chosen->first && program[at + 1] == chosen->second) {
            const usize index = program[at + 3];
            require(index < 8, "invalid difficulty index");
            const usize value = 0xe00a9888U - kProgramBase + index * 4;
            range(program, value, 4);
            const f32 scale = real(program, value);
            require(scale >= 0 && scale <= 100, "invalid difficulty scale");
            return scale;
        }
    }
    throw FormatError("N64 Legends traps: unterminated difficulty table");
}
} // namespace gdl::formats
