#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"
#include "engine/render/Mesh.h"

#include "TestSupport.h"
#include "formats/GeometryStream.h"
#include "formats/ModelArchive.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using Catch::Approx;

/** Lays a stream out word by word, so vertex data can be placed at exact word offsets. */
class StreamBuilder {
public:
    void word(std::size_t index, std::uint32_t value) {
        ensure(index + 1);
        m_words[index] = value;
    }
    void shortAt(std::size_t wordIndex, std::size_t byteOffset, std::int16_t value) {
        ensure(wordIndex + byteOffset / 4 + 1);
        m_bytes[wordIndex * 4 + byteOffset] =
            static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) & 0xFFU);
        m_bytes[wordIndex * 4 + byteOffset + 1] =
            static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8U);
    }
    std::vector<std::uint8_t> finish(std::size_t totalWords) {
        ensure(totalWords);
        for (std::size_t i = 0; i < m_words.size(); ++i) {
            if (m_words[i] != 0) {
                for (std::size_t b = 0; b < 4; ++b) {
                    m_bytes[i * 4 + b] = static_cast<std::uint8_t>((m_words[i] >> (8 * b)) & 0xFFU);
                }
            }
        }
        return m_bytes;
    }

private:
    void ensure(std::size_t words) {
        if (m_words.size() < words) {
            m_words.resize(words, 0);
            m_bytes.resize(words * 4, 0);
        }
    }
    std::vector<std::uint32_t> m_words;
    std::vector<std::uint8_t> m_bytes;
};

std::uint16_t packNormal(int x, int y, int z, bool kick) {
    return static_cast<std::uint16_t>((x + 15) | ((y + 15) << 5) | ((z + 15) << 10) |
                                      (kick ? 0x8000 : 0));
}

/** One packet of five short-position vertices whose fourth vertex restarts the strip;
 * `lightmapped` gives each vertex four texture values, the lightmap's pair after its own,
 * and `flat` sets the flag that starts the packet's strips wound the other way. */
std::vector<std::uint8_t> samplePacket(bool lightmapped = false, bool flat = true) {
    StreamBuilder b;
    b.word(0, lightmapped ? 8 : 7); // quadwords in total, header included
    b.word(2, 0x6C018000);
    b.word(3, 5);
    b.word(5, flat ? 0x3F800000 : 0);
    b.word(7, 0x69000000);
    const std::array<std::array<std::int16_t, 3>, 5> kPositions{
        {{128, 0, 0}, {0, 128, 0}, {0, 0, 128}, {256, 0, 0}, {0, 256, 0}}};
    for (std::size_t v = 0; v < 5; ++v) {
        for (std::size_t k = 0; k < 3; ++k) {
            b.shortAt(8, v * 6 + k * 2, kPositions[v][k]);
        }
    }
    b.word(17, 0x6F058002);
    const std::array<std::uint16_t, 5> kNormals{
        packNormal(0, 0, 15, false), packNormal(15, 0, 0, false), packNormal(0, -15, 0, false),
        packNormal(0, 0, 15, true), packNormal(0, 0, 15, false)};
    for (std::size_t v = 0; v < 5; ++v) {
        b.shortAt(18, v * 2, static_cast<std::int16_t>(kNormals[v]));
    }
    if (lightmapped) {
        b.word(21, 0x6D000000);
        for (std::size_t v = 0; v < 5; ++v) {
            b.shortAt(22, v * 8, static_cast<std::int16_t>(64 * v));
            b.shortAt(22, v * 8 + 2, 128);
            b.shortAt(22, v * 8 + 4, static_cast<std::int16_t>(32 * v));
            b.shortAt(22, v * 8 + 6, 256);
        }
        b.word(32, 0x17000000);
        b.word(33, 0);
        return b.finish(36);
    }
    b.word(21, 0x65000000);
    for (std::size_t v = 0; v < 5; ++v) {
        b.shortAt(22, v * 4, static_cast<std::int16_t>(64 * v));
        b.shortAt(22, v * 4 + 2, 128);
    }
    b.word(27, 0x17000000);
    b.word(28, 0);
    return b.finish(32);
}

TEST_CASE("packets decode into strips split at kick vertices", "[formats][geometry]") {
    Mesh mesh;
    decodeGeometryStream(samplePacket(), 42, mesh);
    REQUIRE(mesh.parts.size() == 1);
    REQUIRE(mesh.parts[0].texture == 42);
    REQUIRE(mesh.vertices.size() == 6);
    REQUIRE(mesh.triangleCount() == 2);
    // The flag is set and both strips start on even vertices, so both are turned; without
    // it they keep their order.
    REQUIRE(mesh.parts[0].indices == std::vector<std::uint32_t>{1, 0, 2, 4, 3, 5});
    Mesh plain;
    decodeGeometryStream(samplePacket(false, false), 42, plain);
    REQUIRE(plain.parts[0].indices == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5});

    REQUIRE(mesh.vertices[0].position == Vec3{1.0f, 0.0f, 0.0f});
    REQUIRE(mesh.vertices[1].position == Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE(mesh.vertices[3].position == Vec3{0.0f, 0.0f, 1.0f}); // vertex 2 again
    REQUIRE(mesh.vertices[4].position == Vec3{2.0f, 0.0f, 0.0f});
    REQUIRE(mesh.vertices[0].normal.z == Approx(1.0f));
    REQUIRE(mesh.vertices[1].normal.x == Approx(1.0f));
    REQUIRE(mesh.vertices[2].normal.y == Approx(-1.0f));
    REQUIRE(mesh.vertices[1].uv.x == Approx(0.5f));
    REQUIRE(mesh.vertices[1].uv.y == Approx(1.0f));
    REQUIRE(mesh.vertices[5].uv.x == Approx(2.0f));
    REQUIRE(mesh.parts[0].lightmap == 0);
    REQUIRE(mesh.vertices[1].lightmapUv == Vec2{0.0f, 0.0f});
}

TEST_CASE("four-value texture coordinates carry the lightmap's pair", "[formats][geometry]") {
    Mesh mesh;
    decodeGeometryStream(samplePacket(true), 42, 507, mesh);
    REQUIRE(mesh.parts.size() == 1);
    REQUIRE(mesh.parts[0].texture == 42);
    REQUIRE(mesh.parts[0].lightmap == 507);
    REQUIRE(mesh.vertices.size() == 6);
    REQUIRE(mesh.vertices[1].uv.x == Approx(0.5f));
    REQUIRE(mesh.vertices[1].uv.y == Approx(1.0f));
    REQUIRE(mesh.vertices[1].lightmapUv.x == Approx(0.25f));
    REQUIRE(mesh.vertices[1].lightmapUv.y == Approx(2.0f));
    REQUIRE(mesh.vertices[5].lightmapUv.x == Approx(1.0f));
}

TEST_CASE("a colour block gives every vertex the lighting baked into it", "[formats][geometry]") {
    // The plain packet again, with a block of five-bit colours between the normals and the
    // texture coordinates: red, green, blue, grey, and black.
    StreamBuilder b;
    b.word(0, 8);
    b.word(2, 0x6C018000);
    b.word(3, 5);
    b.word(5, 0x3F800000);
    b.word(7, 0x69000000);
    const std::array<std::array<std::int16_t, 3>, 5> kPositions{
        {{128, 0, 0}, {0, 128, 0}, {0, 0, 128}, {256, 0, 0}, {0, 256, 0}}};
    for (std::size_t v = 0; v < 5; ++v) {
        for (std::size_t k = 0; k < 3; ++k) {
            b.shortAt(8, v * 6 + k * 2, kPositions[v][k]);
        }
    }
    b.word(17, 0x6F058002);
    for (std::size_t v = 0; v < 5; ++v) {
        b.shortAt(18, v * 2, static_cast<std::int16_t>(packNormal(0, 0, 15, false)));
    }
    b.word(21, 3);
    const std::array<std::uint16_t, 5> kColors{0x001F, 0x03E0, 0x7C00, 0x4210, 0x0000};
    for (std::size_t v = 0; v < 5; ++v) {
        b.shortAt(22, v * 2, static_cast<std::int16_t>(kColors[v]));
    }
    b.word(25, 0x65000000);
    for (std::size_t v = 0; v < 5; ++v) {
        b.shortAt(26, v * 4, static_cast<std::int16_t>(64 * v));
        b.shortAt(26, v * 4 + 2, 128);
    }
    b.word(31, 0x17000000);
    b.word(32, 0);
    Mesh mesh;
    decodeGeometryStream(b.finish(36), 4, mesh);
    REQUIRE(mesh.prelit);
    REQUIRE(mesh.vertices.size() == 5);
    REQUIRE(mesh.vertices[0].color == Color::rgba(248, 0, 0, 255));
    REQUIRE(mesh.vertices[1].color == Color::rgba(0, 248, 0, 255));
    REQUIRE(mesh.vertices[2].color == Color::rgba(0, 0, 248, 255));
    REQUIRE(mesh.vertices[3].color == Color::rgba(128, 128, 128, 255));
    REQUIRE(mesh.vertices[4].color == Color::rgba(0, 0, 0, 255));
    REQUIRE(mesh.vertices[1].uv.x == Approx(0.5f)); // the coordinates still follow the block
    // Without the block a mesh is lit by the lights and its vertices stay white.
    Mesh plain;
    decodeGeometryStream(samplePacket(), 4, plain);
    REQUIRE_FALSE(plain.prelit);
    REQUIRE(plain.vertices[0].color == Color::white());
}

TEST_CASE("broken streams are rejected", "[formats][geometry]") {
    Mesh mesh;
    REQUIRE_THROWS_AS(decodeGeometryStream(std::vector<std::uint8_t>(4, 0), 0, mesh), FormatError);
    std::vector<std::uint8_t> tooShort = samplePacket();
    tooShort.resize(64);
    REQUIRE_THROWS_AS(decodeGeometryStream(tooShort, 0, mesh), FormatError);
    std::vector<std::uint8_t> claimsMore = samplePacket();
    claimsMore[0] = 20;
    REQUIRE_THROWS_AS(decodeGeometryStream(claimsMore, 0, mesh), FormatError);
}

TEST_CASE("the menu arrow decodes within its bounding radius", "[formats][geometry][assets]") {
    const auto file = test::assetOrSkip("POWERUPS/objects.ngc");
    const ModelArchive archive = ModelArchive::parse(readFile(file));
    const auto index = archive.findObject("ICON_ARROWFR#0");
    REQUIRE(index.has_value());
    const ArchiveObject& object = archive.objects()[*index];
    Mesh mesh;
    for (const ArchiveSubObject& sub : object.subObjects) {
        decodeGeometryStream(sub.geometry, sub.textureIndex, mesh);
    }
    REQUIRE(mesh.parts.size() == 1);
    REQUIRE(mesh.parts[0].texture == 326);
    REQUIRE(mesh.triangleCount() == 61);
    for (const MeshVertex& v : mesh.vertices) {
        REQUIRE(glm::length(v.position) <= object.boundingRadius + 0.01f);
        REQUIRE(glm::length(v.normal) <= 1.8f);
    }
}

} // namespace
