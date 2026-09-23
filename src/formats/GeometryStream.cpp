#include "formats/GeometryStream.h"

#include <bit>
#include <format>
#include <vector>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr u32 kPositionShort = 0x69;
constexpr u32 kPositionByte = 0x6A;
constexpr u32 kTexcoordQuad = 0x6D;
constexpr u32 kTexcoordByte = 0x66;
constexpr u32 kColorBlockCode = 3;      ///< a packed colour per vertex, for prelit objects
constexpr u32 kColorChannelMask = 0x1F; ///< five bits a channel, red lowest
constexpr u32 kColorShift = 3;          ///< spread to eight bits the way the console does
constexpr f32 kPositionScale = 1.0f / 128.0f;
constexpr f32 kTexcoordScale = 1.0f / 128.0f;
constexpr f32 kNormalScale = 1.0f / 15.0f;
constexpr s32 kNormalBias = 15;
constexpr u32 kNormalMask = 0x1F;
constexpr u16 kKickBit = 0x8000;
constexpr usize kWordBytes = 4;

class WordStream {
public:
    explicit WordStream(std::span<const u8> bytes) : m_bytes(bytes) {}

    usize wordCount() const { return m_bytes.size() / kWordBytes; }

    u32 word(usize index) const {
        if (index >= wordCount()) {
            throw FormatError(std::format("geometry stream reads past its end at word {}", index));
        }
        return readU32LE(m_bytes, index * kWordBytes);
    }

    std::span<const u8> bytesAt(usize offset, usize count) const {
        if (offset > m_bytes.size() || count > m_bytes.size() - offset) {
            throw FormatError("geometry stream vertex data runs past its end");
        }
        return m_bytes.subspan(offset, count);
    }

private:
    std::span<const u8> m_bytes;
};

struct StripVertex {
    MeshVertex vertex;
    bool kick = false;
};

Vec3 unpackNormal(u16 packed) {
    const auto component = [packed](u32 shift) {
        return static_cast<f32>(static_cast<s32>((packed >> shift) & kNormalMask) - kNormalBias) *
               kNormalScale;
    };
    return Vec3{component(0), component(5), component(10)};
}

/** Appends a strip as triangles wound the way the console keeps them: the strip's own
 * alternation undone, and the whole strip turned when `flip` says its first face is wound
 * the other way. */
void appendStrip(std::span<const StripVertex> strip, u32 base, bool flip, Mesh& mesh,
                 MeshPart& part) {
    if (strip.size() < 3) {
        return;
    }
    for (usize k = 0; k + 2 < strip.size(); ++k) {
        const u32 a = base + static_cast<u32>(k);
        const u32 b = a + 1;
        const u32 c = a + 2;
        if ((k % 2 == 0) != flip) {
            part.indices.insert(part.indices.end(), {a, b, c});
        } else {
            part.indices.insert(part.indices.end(), {b, a, c});
        }
    }
    for (const StripVertex& v : strip) {
        mesh.vertices.push_back(v.vertex);
    }
}

/** Splits a packet's vertices into strips wherever a vertex carries the kick bit. The
 * original culls each strip by the parity of its first vertex, which alternates from the
 * packet's `flat` flag, so a strip starting on the other parity is wound the other way. */
void emitPacket(const std::vector<StripVertex>& vertices, bool flatFirst, Mesh& mesh,
                MeshPart& part) {
    const auto flipAt = [flatFirst](usize start) { return flatFirst != (start % 2 == 1); };
    usize start = 0;
    for (usize j = 0; j < vertices.size(); ++j) {
        if (vertices[j].kick && j - start > 1) {
            appendStrip(std::span(vertices).subspan(start, j - start),
                        static_cast<u32>(mesh.vertices.size()), flipAt(start), mesh, part);
            start = j - 1;
        }
    }
    appendStrip(std::span(vertices).subspan(start), static_cast<u32>(mesh.vertices.size()),
                flipAt(start), mesh, part);
}

} // namespace

void decodeGeometryStream(std::span<const u8> stream, u32 texture, Mesh& mesh) {
    decodeGeometryStream(stream, texture, 0, mesh);
}

void decodeGeometryStream(std::span<const u8> stream, u32 texture, u32 lightmap, Mesh& mesh) {
    const WordStream words(stream);
    if (words.wordCount() < 2) {
        throw FormatError("geometry stream is too short for its header");
    }
    const usize end = (usize{words.word(0) & 0xFFFFU} + 1) * 4;
    if (end > words.wordCount()) {
        throw FormatError("geometry stream header claims more data than present");
    }
    MeshPart part;
    part.texture = texture;
    part.lightmap = lightmap;

    usize idx = 2;
    std::vector<StripVertex> packet;
    while (idx < end) {
        if (words.word(idx) == 0) {
            break;
        }
        const usize count = words.word(idx + 1);
        const bool flatFirst = std::bit_cast<f32>(words.word(idx + 3)) == 1.0f;
        const u32 positionFormat = words.word(idx + 5) >> 24U;
        const usize positionAt = (idx + 6) * kWordBytes;
        idx += 5;
        usize positionStride = 0;
        if (positionFormat == kPositionShort) {
            idx += (((count + 1) * 0x30 + 0x1F) >> 5U) + 1;
            positionStride = 6;
        } else if (positionFormat == kPositionByte) {
            idx += (((count + 1) * 0x18 + 0x1F) >> 5U) + 1;
            positionStride = 3;
        } else {
            idx += (count + 1) * 3 + 1;
            positionStride = 12;
        }
        const usize step = (((count << 4U) + 0x1F) >> 5U) + 1;
        const usize normalAt = (idx + 1) * kWordBytes;
        usize next = idx + step;
        usize colorAt = 0;
        bool prelit = false;
        if ((words.word(next) & 0xFFFU) == kColorBlockCode) {
            colorAt = (next + 1) * kWordBytes;
            prelit = true;
            next += step;
        }
        const u32 texcoordFormat = words.word(next) >> 24U;
        const usize texcoordAt = (next + 1) * kWordBytes;
        usize texcoordStride = 4;
        if (texcoordFormat == kTexcoordQuad) {
            texcoordStride = 8;
            next += count * 2 + 1;
        } else if (texcoordFormat == kTexcoordByte) {
            texcoordStride = 2;
            next += step;
        } else {
            next += count + 1;
        }
        words.word(next);
        idx = next + 1;

        const std::span<const u8> positions = words.bytesAt(positionAt, count * positionStride);
        const std::span<const u8> normals = words.bytesAt(normalAt, count * 2);
        const std::span<const u8> texcoords = words.bytesAt(texcoordAt, count * texcoordStride);
        const std::span<const u8> colors =
            prelit ? words.bytesAt(colorAt, count * 2) : std::span<const u8>{};
        packet.clear();
        for (usize v = 0; v < count; ++v) {
            StripVertex sv;
            const usize p = v * positionStride;
            if (positionFormat == kPositionShort) {
                for (usize k = 0; k < 3; ++k) {
                    sv.vertex.position[static_cast<s32>(k)] =
                        static_cast<f32>(static_cast<s16>(readU16LE(positions, p + k * 2))) *
                        kPositionScale;
                }
            } else if (positionFormat == kPositionByte) {
                for (usize k = 0; k < 3; ++k) {
                    sv.vertex.position[static_cast<s32>(k)] =
                        static_cast<f32>(static_cast<s8>(positions[p + k])) * kPositionScale;
                }
            } else {
                for (usize k = 0; k < 3; ++k) {
                    sv.vertex.position[static_cast<s32>(k)] =
                        static_cast<f32>(static_cast<s32>(readU32LE(positions, p + k * 4))) *
                        kPositionScale;
                }
            }
            const u16 packedNormal = readU16LE(normals, v * 2);
            sv.vertex.normal = unpackNormal(packedNormal);
            sv.kick = (packedNormal & kKickBit) != 0;
            if (prelit) {
                const u16 packed = readU16LE(colors, v * 2);
                sv.vertex.color = Color::rgba(
                    static_cast<u8>((packed & kColorChannelMask) << kColorShift),
                    static_cast<u8>(((packed >> 5U) & kColorChannelMask) << kColorShift),
                    static_cast<u8>(((packed >> 10U) & kColorChannelMask) << kColorShift), 255);
                mesh.prelit = true;
            }
            const usize t = v * texcoordStride;
            if (texcoordFormat == kTexcoordByte) {
                sv.vertex.uv = Vec2{static_cast<f32>(texcoords[t]) * kTexcoordScale,
                                    static_cast<f32>(texcoords[t + 1]) * kTexcoordScale};
            } else {
                sv.vertex.uv = Vec2{static_cast<f32>(readU16LE(texcoords, t)) * kTexcoordScale,
                                    static_cast<f32>(readU16LE(texcoords, t + 2)) * kTexcoordScale};
                if (texcoordFormat == kTexcoordQuad) {
                    sv.vertex.lightmapUv =
                        Vec2{static_cast<f32>(readU16LE(texcoords, t + 4)) * kTexcoordScale,
                             static_cast<f32>(readU16LE(texcoords, t + 6)) * kTexcoordScale};
                }
            }
            packet.push_back(sv);
        }
        emitPacket(packet, flatFirst, mesh, part);
    }
    if (!part.indices.empty()) {
        mesh.parts.push_back(std::move(part));
    }
}

} // namespace gdl::formats
