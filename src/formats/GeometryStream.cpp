#include "formats/GeometryStream.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr std::uint32_t kPositionShort = 0x69;
constexpr std::uint32_t kPositionByte = 0x6A;
constexpr std::uint32_t kTexcoordQuad = 0x6D;
constexpr std::uint32_t kTexcoordByte = 0x66;
constexpr std::uint32_t kColorBlockCode = 3; ///< a packed colour per vertex, for prelit objects
constexpr std::uint32_t kColorChannelMask = 0x1F; ///< five bits a channel, red lowest
constexpr std::uint32_t kColorShift = 3;          ///< spread to eight bits the way the console does
constexpr float kPositionScale = 1.0f / 128.0f;
constexpr float kTexcoordScale = 1.0f / 128.0f;
constexpr float kNormalScale = 1.0f / 15.0f;
constexpr std::int32_t kNormalBias = 15;
constexpr std::uint32_t kNormalMask = 0x1F;
constexpr std::uint16_t kKickBit = 0x8000;
constexpr std::size_t kWordBytes = 4;

class WordStream {
public:
    explicit WordStream(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

    std::size_t wordCount() const { return m_bytes.size() / kWordBytes; }

    std::uint32_t word(std::size_t index) const {
        if (index >= wordCount()) {
            throw FormatError(std::format("geometry stream reads past its end at word {}", index));
        }
        return readU32LE(m_bytes, index * kWordBytes);
    }

    std::span<const std::uint8_t> bytesAt(std::size_t offset, std::size_t count) const {
        if (offset > m_bytes.size() || count > m_bytes.size() - offset) {
            throw FormatError("geometry stream vertex data runs past its end");
        }
        return m_bytes.subspan(offset, count);
    }

private:
    std::span<const std::uint8_t> m_bytes;
};

struct StripVertex {
    MeshVertex vertex;
    bool kick = false;
};

Vec3 unpackNormal(std::uint16_t packed) {
    const auto component = [packed](std::uint32_t shift) {
        return static_cast<float>(static_cast<std::int32_t>((packed >> shift) & kNormalMask) -
                                  kNormalBias) *
               kNormalScale;
    };
    return Vec3{component(0), component(5), component(10)};
}

/** Appends a strip as triangles wound the way the console keeps them: the strip's own
 * alternation undone, and the whole strip turned when `flip` says its first face is wound
 * the other way. */
void appendStrip(std::span<const StripVertex> strip, std::uint32_t base, bool flip, Mesh& mesh,
                 MeshPart& part) {
    if (strip.size() < 3) {
        return;
    }
    for (std::size_t k = 0; k + 2 < strip.size(); ++k) {
        const std::uint32_t a = base + static_cast<std::uint32_t>(k);
        const std::uint32_t b = a + 1;
        const std::uint32_t c = a + 2;
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
    const auto flipAt = [flatFirst](std::size_t start) { return flatFirst != (start % 2 == 1); };
    std::size_t start = 0;
    for (std::size_t j = 0; j < vertices.size(); ++j) {
        if (vertices[j].kick && j - start > 1) {
            appendStrip(std::span(vertices).subspan(start, j - start),
                        static_cast<std::uint32_t>(mesh.vertices.size()), flipAt(start), mesh,
                        part);
            start = j - 1;
        }
    }
    appendStrip(std::span(vertices).subspan(start),
                static_cast<std::uint32_t>(mesh.vertices.size()), flipAt(start), mesh, part);
}

} // namespace

void decodeGeometryStream(std::span<const std::uint8_t> stream, std::uint32_t texture, Mesh& mesh) {
    decodeGeometryStream(stream, texture, 0, mesh);
}

void decodeGeometryStream(std::span<const std::uint8_t> stream, std::uint32_t texture,
                          std::uint32_t lightmap, Mesh& mesh) {
    const WordStream words(stream);
    if (words.wordCount() < 2) {
        throw FormatError("geometry stream is too short for its header");
    }
    const std::size_t end = (std::size_t{words.word(0) & 0xFFFFU} + 1) * 4;
    if (end > words.wordCount()) {
        throw FormatError("geometry stream header claims more data than present");
    }
    MeshPart part;
    part.texture = texture;
    part.lightmap = lightmap;

    std::size_t idx = 2;
    std::vector<StripVertex> packet;
    while (idx < end) {
        if (words.word(idx) == 0) {
            break;
        }
        const std::size_t count = words.word(idx + 1);
        const bool flatFirst = std::bit_cast<float>(words.word(idx + 3)) == 1.0f;
        const std::uint32_t positionFormat = words.word(idx + 5) >> 24U;
        const std::size_t positionAt = (idx + 6) * kWordBytes;
        idx += 5;
        std::size_t positionStride = 0;
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
        const std::size_t step = (((count << 4U) + 0x1F) >> 5U) + 1;
        const std::size_t normalAt = (idx + 1) * kWordBytes;
        std::size_t next = idx + step;
        std::size_t colorAt = 0;
        bool prelit = false;
        if ((words.word(next) & 0xFFFU) == kColorBlockCode) {
            colorAt = (next + 1) * kWordBytes;
            prelit = true;
            next += step;
        }
        const std::uint32_t texcoordFormat = words.word(next) >> 24U;
        const std::size_t texcoordAt = (next + 1) * kWordBytes;
        std::size_t texcoordStride = 4;
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

        const std::span<const std::uint8_t> positions =
            words.bytesAt(positionAt, count * positionStride);
        const std::span<const std::uint8_t> normals = words.bytesAt(normalAt, count * 2);
        const std::span<const std::uint8_t> texcoords =
            words.bytesAt(texcoordAt, count * texcoordStride);
        const std::span<const std::uint8_t> colors =
            prelit ? words.bytesAt(colorAt, count * 2) : std::span<const std::uint8_t>{};
        packet.clear();
        for (std::size_t v = 0; v < count; ++v) {
            StripVertex sv;
            const std::size_t p = v * positionStride;
            if (positionFormat == kPositionShort) {
                for (std::size_t k = 0; k < 3; ++k) {
                    sv.vertex.position[static_cast<int>(k)] =
                        static_cast<float>(
                            static_cast<std::int16_t>(readU16LE(positions, p + k * 2))) *
                        kPositionScale;
                }
            } else if (positionFormat == kPositionByte) {
                for (std::size_t k = 0; k < 3; ++k) {
                    sv.vertex.position[static_cast<int>(k)] =
                        static_cast<float>(static_cast<std::int8_t>(positions[p + k])) *
                        kPositionScale;
                }
            } else {
                for (std::size_t k = 0; k < 3; ++k) {
                    sv.vertex.position[static_cast<int>(k)] =
                        static_cast<float>(
                            static_cast<std::int32_t>(readU32LE(positions, p + k * 4))) *
                        kPositionScale;
                }
            }
            const std::uint16_t packedNormal = readU16LE(normals, v * 2);
            sv.vertex.normal = unpackNormal(packedNormal);
            sv.kick = (packedNormal & kKickBit) != 0;
            if (prelit) {
                const std::uint16_t packed = readU16LE(colors, v * 2);
                sv.vertex.color = Color::rgba(
                    static_cast<std::uint8_t>((packed & kColorChannelMask) << kColorShift),
                    static_cast<std::uint8_t>(((packed >> 5U) & kColorChannelMask) << kColorShift),
                    static_cast<std::uint8_t>(((packed >> 10U) & kColorChannelMask) << kColorShift),
                    255);
                mesh.prelit = true;
            }
            const std::size_t t = v * texcoordStride;
            if (texcoordFormat == kTexcoordByte) {
                sv.vertex.uv = Vec2{static_cast<float>(texcoords[t]) * kTexcoordScale,
                                    static_cast<float>(texcoords[t + 1]) * kTexcoordScale};
            } else {
                sv.vertex.uv =
                    Vec2{static_cast<float>(readU16LE(texcoords, t)) * kTexcoordScale,
                         static_cast<float>(readU16LE(texcoords, t + 2)) * kTexcoordScale};
                if (texcoordFormat == kTexcoordQuad) {
                    sv.vertex.lightmapUv =
                        Vec2{static_cast<float>(readU16LE(texcoords, t + 4)) * kTexcoordScale,
                             static_cast<float>(readU16LE(texcoords, t + 6)) * kTexcoordScale};
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
