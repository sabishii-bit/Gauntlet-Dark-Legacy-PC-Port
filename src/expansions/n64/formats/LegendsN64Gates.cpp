#include "expansions/n64/formats/LegendsN64Gates.h"

#include <bit>
#include <cmath>
#include <format>
#include <map>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw FormatError("N64 Legends gates: " + std::string(message));
    }
}
void range(std::span<const u8> bytes, usize at, usize length) {
    require(at <= bytes.size() && length <= bytes.size() - at, "truncated data");
}
s16 integer(std::span<const u8> bytes, usize at) {
    return std::bit_cast<s16>(readU16BE(bytes, at));
}
f32 real(std::span<const u8> bytes, usize at) {
    const f32 value = std::bit_cast<f32>(readU32BE(bytes, at));
    require(std::isfinite(value), "non-finite value");
    return value;
}
std::string sequence(std::span<const u8> bytes, usize at) {
    if (readU32BE(bytes, at) == 0) {
        return {};
    }
    std::string result;
    for (usize i = 0; i < 4; ++i) {
        require(bytes[at + i] >= 32 && bytes[at + i] < 127, "invalid sequence");
        result.push_back(static_cast<char>(bytes[at + i]));
    }
    return result;
}
LegendsN64GateCollider collider(std::span<const u8> bytes, usize at, f32 scale, bool frame) {
    LegendsN64GateCollider result;
    result.shape = readU32BE(bytes, at);
    if (result.shape == 5) {
        return result; // Native ignores the unused dimensions/offsets entirely.
    }
    require(result.shape == 1 || result.shape == 3, "unsupported key gate shape");
    result.dimensions =
        Vec3{real(bytes, at + 4), real(bytes, at + 8), real(bytes, at + 12)} * scale;
    require(std::isfinite(result.dimensions.x) && std::isfinite(result.dimensions.y) &&
                std::isfinite(result.dimensions.z) && result.dimensions.x > 0 &&
                result.dimensions.y > 0 && result.dimensions.z >= 0 &&
                (result.shape != 3 || result.dimensions.z > 0) && result.dimensions.x < 1024 &&
                result.dimensions.y < 1024 && result.dimensions.z < 1024,
            "invalid key gate dimensions");
    if (frame) {
        // e00694e4 scales dimensions, but not the separate frame positions.
        result.offset = Vec2{real(bytes, at + 16), real(bytes, at + 20)};
        require(std::isfinite(result.offset.x) && std::isfinite(result.offset.y),
                "invalid frame offset");
    }
    return result;
}
} // namespace

LegendsN64Gates decodeLegendsN64Gates(std::span<const u8> placements,
                                      std::span<const u8> templates) {
    range(placements, 0, 92);
    LegendsN64Gates result;
    const usize count = readU16BE(placements, 42);
    if (count == 0) {
        return result;
    }
    const usize offset = readU32BE(placements, 12);
    require(offset >= 92 && count <= 16384, "invalid placement table");
    range(placements, offset, count * 24);
    range(templates, 0, 4);
    const usize templateCount = readU32BE(templates, 0);
    require(templateCount <= 4096, "template budget exceeded");
    range(templates, 4, templateCount * 96);
    std::map<u16, usize> lookup;
    for (usize i = 0; i < templateCount; ++i) {
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
        if (kind != 3 && kind != 4) {
            continue;
        }
        // e0069c90 admits keys only for family08 and idle type3/4 traps.
        if ((key >> 8U) != 8 || (readU32BE(templates, native + 4) & 0x07000000U) != 0x02000000U) {
            result.warnings.push_back(std::format("Native gate {} has unsupported activation.", i));
            continue;
        }
        LegendsN64Gate gate;
        gate.nativeIndex = static_cast<u32>(i);
        gate.templateKey = key;
        gate.flags = flags;
        gate.placementFlags = placements[at + 10];
        gate.minimumPlayers = placements[at + 11];
        gate.position = Vec3{integer(placements, at), integer(placements, at + 2),
                             integer(placements, at + 4)} /
                        4.0f;
        gate.yaw = static_cast<f32>(integer(placements, at + 6)) / 32;
        gate.scale = real(placements, at + 16);
        gate.scale = gate.scale > .001f ? gate.scale : 1;
        require(gate.scale <= 64, "invalid gate scale");
        gate.groundProjected = (flags & 0x20U) == 0;
        gate.idleSequence = sequence(templates, native + 12);
        gate.openingSequence = sequence(templates, native + 16);
        gate.sound = readU32BE(templates, native + 28);
        const f32 collisionScale = gate.scale * ((gate.placementFlags & 8U) != 0 ? 4.0f : 1.0f);
        gate.primary = collider(templates, native + 32, collisionScale, false);
        for (usize frame = 0; frame < gate.frames.size(); ++frame) {
            gate.frames[frame] =
                collider(templates, native + 48 + frame * 24, collisionScale, true);
        }
        result.gates.push_back(std::move(gate));
    }
    return result;
}
} // namespace gdl::formats
