#include "expansions/n64/formats/LegendsN64Morph.h"

#include <array>
#include <bit>
#include <cmath>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

void require(bool valid, const char* message) {
    if (!valid) {
        throw FormatError(message);
    }
}

} // namespace

usize legendsN64VariantGeometryBytes(const Mesh& mesh, usize sourceVertices, usize frames,
                                     usize remainingBytes) {
    usize bytes = 0;
    const auto add = [&](usize count, usize stride) {
        require(count <= (remainingBytes - bytes) / stride,
                "N64 Legends variant: geometry byte budget exceeded");
        bytes += count * stride;
    };
    add(1, sizeof(Mesh));
    add(1, sizeof(std::vector<u32>));
    add(mesh.vertices.size(), sizeof(MeshVertex));
    add(mesh.parts.size(), sizeof(MeshPart));
    for (const auto& part : mesh.parts) {
        add(part.indices.size(), sizeof(u32));
    }
    add(sourceVertices, sizeof(u32));
    require(frames != 0 && frames <= remainingBytes / bytes,
            "N64 Legends variant: geometry byte budget exceeded");
    return bytes * frames;
}

namespace {
std::vector<Mesh> decodeAbsoluteFrames(std::span<const u8> data, usize frames, usize keys,
                                       bool attributes, const std::array<s16, 3>& base,
                                       u16 shiftFlags, std::span<const u8> binding,
                                       const Mesh& original, std::span<const u32> vertexSources,
                                       usize& remainingVertices, usize& remainingBytes,
                                       bool delta = false) {
    require(frames != 0 && frames <= 4096 && keys != 0,
            "N64 Legends morph: invalid frame/key count");
    require(binding.size() >= 2, "N64 Legends morph: missing vertex binding");
    const usize nativeVertices = readU16BE(binding, 0);
    require(nativeVertices != 0 && nativeVertices <= (binding.size() - 2) / 2,
            "N64 Legends morph: truncated vertex binding");
    require(!original.vertices.empty() && vertexSources.size() == original.vertices.size(),
            "N64 Legends morph: missing source vertex identity");
    require(original.vertices.size() <= remainingVertices / frames,
            "N64 Legends morph: decoded vertex budget exceeded");
    const usize byteCharge =
        legendsN64VariantGeometryBytes(original, vertexSources.size(), frames, remainingBytes);
    const usize stride = delta ? (attributes ? 4 : 2) : (attributes ? 6 : 3);
    const usize frameBytes = keys * stride;
    require(frames <= data.size() / frameBytes, "N64 Legends morph: truncated frame keys");
    std::vector<u16> indices;
    indices.reserve(vertexSources.size());
    for (const u32 source : vertexSources) {
        require(source < nativeVertices, "N64 Legends morph: source vertex outside binding");
        const u16 key = readU16BE(binding, 2 + static_cast<usize>(source) * 2);
        require(key < keys, "N64 Legends morph: vertex key outside frame");
        indices.push_back(key);
    }
    const std::array<u32, 3> shifts{(shiftFlags >> 10U) & 31U, (shiftFlags >> 5U) & 31U,
                                    shiftFlags & 31U};
    std::vector<Mesh> result;
    result.reserve(frames);
    // e0029d34 writes native signed16 XYZ and optional RGB/normal bytes into
    // the original Vtx array. UVs, alpha, material indices and topology stay.
    for (usize frame = 0; frame < frames; ++frame) {
        Mesh mesh = original;
        for (usize index = 0; index < mesh.vertices.size(); ++index) {
            const usize at = frame * frameBytes + static_cast<usize>(indices[index]) * stride;
            auto& vertex = mesh.vertices[index];
            for (usize axis = 0; axis < base.size(); ++axis) {
                u32 expanded = 0;
                if (delta) {
                    // e0031f88/property6 and e00321ec/property16 replay frame0..N
                    // after e0029cc0 restores the
                    // original signed16 XYZ. e0029d34 adds unsigned packed
                    // 5/5/6-bit increments plus the signed per-axis bias.
                    const u32 packed = readU16BE(data, at);
                    expanded = axis == 0                    ? (packed & 31U) << shifts[0]
                               : axis == 1 && shifts[1] < 5 ? (packed & 0x3e0U) >> (5 - shifts[1])
                               : axis == 1                  ? (packed & 0x3e0U) << (shifts[1] - 5)
                                           : (packed & 0xfc00U) >> ((10 - shifts[2]) & 31U);
                    const f32 previous = (frame == 0 ? original : result.back())
                                             .vertices[index]
                                             .position[static_cast<int>(axis)] *
                                         64;
                    require(std::isfinite(previous) && previous >= -32768 && previous <= 32767,
                            "N64 Legends morph: invalid original fixed-point position");
                    expanded += static_cast<u32>(static_cast<s32>(previous));
                } else {
                    expanded = static_cast<u32>(data[at + axis]) << shifts[axis];
                }
                expanded += static_cast<u32>(static_cast<s32>(base[axis]));
                vertex.position[static_cast<int>(axis)] =
                    static_cast<f32>(std::bit_cast<s16>(static_cast<u16>(expanded))) / 64.0f;
            }
            if (attributes) {
                std::array<u8, 3> values{};
                if (delta) {
                    const u16 packed = readU16BE(data, at + 2);
                    values = {static_cast<u8>((packed & 31U) << 3U),
                              static_cast<u8>((packed & 0x3e0U) >> 2U),
                              static_cast<u8>((packed >> 8U) & 0xfcU)};
                } else {
                    values = {data[at + 3], data[at + 4], data[at + 5]};
                }
                if (mesh.prelit) {
                    vertex.color.r = values[0];
                    vertex.color.g = values[1];
                    vertex.color.b = values[2];
                } else {
                    for (usize axis = 0; axis < base.size(); ++axis) {
                        vertex.normal[static_cast<int>(axis)] =
                            static_cast<f32>(std::bit_cast<s8>(values[axis])) / 127.0f;
                    }
                }
            }
        }
        result.push_back(std::move(mesh));
    }
    remainingVertices -= original.vertices.size() * frames;
    remainingBytes -= byteCharge;
    return result;
}
} // namespace

std::optional<std::vector<Mesh>>
decodeLegendsN64Morph(std::span<const u8> track, std::span<const u8> binding, const Mesh& original,
                      std::span<const u32> vertexSources, usize& remainingVertices,
                      usize& remainingBytes) {
    require(track.size() >= 12, "N64 Legends morph: truncated header");
    const u16 framesAndFlags = readU16BE(track, 0);
    const usize frames = framesAndFlags & 0x7fffU;
    const usize keys = readU16BE(track, 2);
    const u16 shiftFlags = readU16BE(track, 10);
    require(frames != 0 && frames <= 4096 && keys != 0,
            "N64 Legends morph: invalid frame/key count");
    std::array<s16, 3> base{};
    for (usize axis = 0; axis < base.size(); ++axis) {
        base[axis] = std::bit_cast<s16>(readU16BE(track, 4 + axis * 2));
    }
    return decodeAbsoluteFrames(track.subspan(12), frames, keys, (framesAndFlags & 0x8000U) != 0,
                                base, shiftFlags, binding, original, vertexSources,
                                remainingVertices, remainingBytes, (shiftFlags & 0x8000U) != 0);
}

std::optional<std::vector<Mesh>>
decodeLegendsN64VertexKeys(std::span<const u8> track, std::span<const u8> binding,
                           const Mesh& original, std::span<const u32> vertexSources,
                           usize& remainingVertices, usize& remainingBytes) {
    // e00321ec -> e0029b00 -> e0029d34: property16's first flag selects packed
    // deltas, the second includes RGB/normals, and its remaining two flags govern
    // visibility before/after the authored run. Byte1 is not consumed here.
    require(track.size() >= 16, "N64 Legends vertex keys: truncated header");
    const usize frames = readU16BE(track, 2);
    const usize keys = readU16BE(track, 6);
    require(frames != 0 && frames <= 4096 && keys != 0,
            "N64 Legends vertex keys: invalid frame/key count");
    if (readU16BE(track, 4) != 0) {
        return std::nullopt;
    }
    std::array<s16, 3> base{};
    for (usize axis = 0; axis < base.size(); ++axis) {
        base[axis] = std::bit_cast<s16>(readU16BE(track, 8 + axis * 2));
    }
    return decodeAbsoluteFrames(track.subspan(16), frames, keys, (track[0] & 2U) != 0, base,
                                readU16BE(track, 14), binding, original, vertexSources,
                                remainingVertices, remainingBytes, (track[0] & 1U) != 0);
}

} // namespace gdl::formats
