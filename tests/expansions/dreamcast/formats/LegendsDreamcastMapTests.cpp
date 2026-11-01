#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/shared/formats/LegendsMap.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value);
    bytes.at(at + 1) = static_cast<u8>(value >> 8U);
}

void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value));
    put16(bytes, at + 2, static_cast<u16>(value >> 16U));
}

void putFloat(std::vector<u8>& bytes, usize at, f32 value) {
    put32(bytes, at, std::bit_cast<u32>(value));
}

void putName(std::vector<u8>& bytes, usize at, std::string_view value) {
    for (usize i = 0; i < value.size(); ++i) {
        bytes.at(at + i) = static_cast<u8>(value[i]);
    }
}

struct Fixture {
    static constexpr usize kRecords = 120;
    static constexpr usize kMesh = kRecords + 60;
    static constexpr usize kLod = 116;
    static constexpr usize kVertices = 324;
    static constexpr usize kTriangles = 420;
    static constexpr usize kDefinitions = 436;
    std::vector<u8> world = std::vector<u8>(240);
    std::vector<u8> model = std::vector<u8>(472);

    Fixture() {
        put32(world, 0, 2);
        put32(world, 4, kRecords);
        for (usize axis = 0; axis < 3; ++axis) {
            putFloat(world, 36 + axis * 4, -100.0f);
            putFloat(world, 48 + axis * 4, 100.0f);
            putFloat(world, kRecords + 28 + axis * 4, 10.0f * static_cast<f32>(axis + 1));
            putFloat(world, kMesh + 28 + axis * 4, static_cast<f32>(axis + 1));
        }
        putName(world, kRecords, "GROUP");
        put16(world, kRecords + 44, 0xffff);
        put16(world, kRecords + 46, 1);
        putName(world, kMesh, "QUAD");
        put32(world, kMesh + 16, 2);
        put32(world, kMesh + 40, 1);
        put16(world, kMesh + 44, 0xffff);
        put16(world, kMesh + 46, 0xffff);
        put32(model, 64, 0xf00b0001U);
        put32(model, 88, kDefinitions);
        put32(model, 96, 1);
        put32(model, 100, 1);
        put16(model, 244 + 4, 64);
        put16(model, 244 + 6, 32);
        put32(model, kLod + 4, 16);
        put32(model, kLod + 8, 4);
        put32(model, kLod + 12, kVertices);
        put32(model, kLod + 16, 2);
        put32(model, kLod + 20, kTriangles);
        put32(model, kLod + 28, 460);
        const std::array<Vec3, 4> vertices{{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}}};
        for (usize i = 0; i < 4; ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                putFloat(model, kVertices + i * 24 + static_cast<usize>(axis) * 4,
                         vertices[i][axis]);
            }
        }
        put16(model, kTriangles, 1);
        put16(model, kTriangles + 2, 2);
        put16(model, kTriangles + 4, 0);
        put16(model, kTriangles + 6, 0xc000);
        put16(model, kTriangles + 8, 3);
        put16(model, kTriangles + 10, 1);
        put16(model, kTriangles + 12, 2);
        putName(model, kDefinitions, "QUAD");
    }
};
} // namespace

TEST_CASE("Dreamcast Legends geometry uses native hierarchy and strip winding",
          "[formats][legends]") {
    const Fixture fixture;
    const LegendsMap map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
    REQUIRE(map.objects.size() == 2);
    CHECK(map.objects[0].mesh.vertices.empty());
    const auto& object = map.objects[1];
    CHECK(object.name == "QUAD");
    CHECK(object.flags == 2);
    CHECK(object.mesh.flags == 0);
    CHECK(object.transform[3] == Vec4(11, 22, 33, 1));
    REQUIRE(object.mesh.vertices.size() == 4);
    CHECK(object.mesh.vertices[3].position == Vec3(1, 0, 1));
    REQUIRE(object.mesh.parts.size() == 1);
    CHECK(object.mesh.parts[0].texture == 0);
    CHECK(object.mesh.parts[0].indices == std::vector<u32>{1, 0, 2, 3, 1, 2});
    CHECK(glm::dot(object.mesh.vertices[0].normal, object.mesh.vertices[3].normal) > 0.99f);
    CHECK_FALSE(map.warnings.empty());
}

TEST_CASE("Dreamcast world keeps sprite flags and per-strip lightmap coordinates",
          "[formats][legends][dreamcast-presentation]") {
    Fixture fixture;
    put32(fixture.world, Fixture::kMesh + 24, 0x01c01880);
    put16(fixture.model, Fixture::kVertices + 16, 512);
    put16(fixture.model, Fixture::kVertices + 18, 256);
    const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
    const auto& object = map.objects[1];
    CHECK(object.dreamcastDrawState);
    CHECK(object.hasNativeDrawState);
    CHECK(object.nativeDrawFlags == 0x01c01880);
    REQUIRE(object.partLightmaps.size() == 1);
    CHECK(object.partLightmaps[0] == 460);
    CHECK(object.mesh.vertices[0].lightmapUv == Vec2{0.5f, 0.25f});
    put32(fixture.model, Fixture::kLod + 28, 471);
    CHECK_THROWS_AS(LegendsMap::parseDreamcast(fixture.world, fixture.model), FormatError);
}

TEST_CASE("Dreamcast Legends geometry rejects malformed extents and topology",
          "[formats][legends]") {
    Fixture fixture;
    SECTION("short world") {
        fixture.world.resize(95);
    }
    SECTION("short model") {
        fixture.model.resize(103);
    }
    SECTION("unsupported version") {
        put32(fixture.model, 64, 0xf00b000dU);
    }
    SECTION("model budget") {
        put32(fixture.model, 96, 0xffffffffU);
    }
    SECTION("definition extent") {
        put32(fixture.model, 88, 0xffffffffU);
    }
    SECTION("definition index") {
        put16(fixture.model, Fixture::kDefinitions + 20, 1);
    }
    SECTION("world table in header") {
        put32(fixture.world, 4, 40);
    }
    SECTION("world table extent") {
        put32(fixture.world, 0, 0xffffffffU);
    }
    SECTION("world bounds") {
        putFloat(fixture.world, 36, 101.0f);
    }
    SECTION("non-finite placement") {
        putFloat(fixture.world, Fixture::kMesh + 28, std::numeric_limits<f32>::infinity());
    }
    SECTION("missing model") {
        putName(fixture.world, Fixture::kMesh, "LOST");
    }
    SECTION("invalid child") {
        put16(fixture.world, Fixture::kRecords + 46, 5);
    }
    SECTION("invalid sibling") {
        put16(fixture.world, Fixture::kMesh + 44, 5);
    }
    SECTION("child sibling cycle") {
        put16(fixture.world, Fixture::kMesh + 44, 1);
    }
    SECTION("parent cycle") {
        put16(fixture.world, Fixture::kMesh + 46, 0);
    }
    SECTION("runtime pointer in serialized world") {
        put32(fixture.world, Fixture::kMesh + 40, 0x8c000000);
    }
    SECTION("unsupported fifo") {
        put32(fixture.model, Fixture::kLod + 4, 8);
    }
    SECTION("vertex extent") {
        put32(fixture.model, Fixture::kLod + 12, 0xfffffff0U);
    }
    SECTION("triangle extent") {
        put32(fixture.model, Fixture::kLod + 20, 0xfffffff0U);
    }
    SECTION("invalid vertex") {
        put16(fixture.model, Fixture::kTriangles, 4);
    }
    SECTION("invalid bitmap") {
        put16(fixture.model, Fixture::kTriangles + 6, 0xc001);
    }
    SECTION("zero bitmap width") {
        put16(fixture.model, 244 + 4, 0);
    }
    SECTION("unterminated strip") {
        put16(fixture.model, Fixture::kTriangles + 14, 0x8000);
    }
    SECTION("non-finite vertex") {
        putFloat(fixture.model, Fixture::kVertices, std::numeric_limits<f32>::quiet_NaN());
    }
    CHECK_THROWS_AS(LegendsMap::parseDreamcast(fixture.world, fixture.model), FormatError);
}

TEST_CASE("Dreamcast strips sharing a base texture retain different lightmaps",
          "[formats][legends][dreamcast-presentation]") {
    Fixture fixture;
    fixture.model.resize(484);
    put16(fixture.model, Fixture::kTriangles + 6, 0x4000);
    put16(fixture.model, Fixture::kTriangles + 14, 0x4000);
    const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
    const auto& object = map.objects[1];
    REQUIRE(object.mesh.parts.size() == 2);
    CHECK(object.mesh.parts[0].texture == object.mesh.parts[1].texture);
    CHECK(object.partLightmaps == std::vector<u32>{460, 472});
}

TEST_CASE("Dreamcast fixture models decode packed vertices and position back-references",
          "[formats][legends][scene]") {
    Fixture fixture;
    put32(fixture.model, Fixture::kLod + 4, 0x404);
    std::fill(fixture.model.begin() + Fixture::kVertices,
              fixture.model.begin() + Fixture::kTriangles, 0);
    const std::array<Vec3, 3> points{{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}};
    for (usize i = 0; i < 3; ++i) {
        const usize at = Fixture::kVertices + i * 12;
        put16(fixture.model, at + 2, 512);
        put16(fixture.model, at + 4, static_cast<u16>(-256));
        for (int axis = 0; axis < 3; ++axis) {
            put16(fixture.model, at + 6 + static_cast<usize>(axis) * 2,
                  static_cast<u16>(points[i][axis] * 256));
        }
    }
    // The last vertex has new UVs but reuses vertex 1's position, 2 entries back.
    fixture.model[Fixture::kVertices + 36] = 2;
    put16(fixture.model, Fixture::kVertices + 38, 1024);
    const auto models = decodeDreamcastModelLibrary(fixture.model);
    REQUIRE(models.size() == 1);
    CHECK(models[0].name == "QUAD");
    CHECK(models[0].flags == 0x404);
    REQUIRE(models[0].mesh.vertices.size() == 4);
    CHECK(models[0].mesh.vertices[2].position == Vec3(0, 0, 1));
    CHECK(models[0].mesh.vertices[3].position == Vec3(1, 0, 0));
    CHECK(models[0].mesh.vertices[0].uv == Vec2(32, -8));
    CHECK(models[0].mesh.vertices[3].uv == Vec2(64, 0));
    fixture.model[Fixture::kVertices] = 1;
    CHECK_THROWS_AS(decodeDreamcastModelLibrary(fixture.model), FormatError);
}

TEST_CASE("Dreamcast fixture model normals come from their native table",
          "[formats][legends][scene]") {
    Fixture fixture;
    put32(fixture.model, Fixture::kLod + 4, 2);
    put32(fixture.model, Fixture::kLod + 28, 460);
    fixture.model.resize(508);
    for (usize i = 0; i < 4; ++i) {
        putFloat(fixture.model, 460 + i * 12, 1.0f);
    }
    const auto models = decodeDreamcastModelLibrary(fixture.model);
    CHECK(models[0].mesh.vertices[0].normal == Vec3(1, 0, 0));
    fixture.model.resize(507);
    CHECK_THROWS_AS(decodeDreamcastModelLibrary(fixture.model), FormatError);
}

TEST_CASE("Dreamcast Legends UVs use native float or fixed-point coordinates",
          "[formats][legends]") {
    Fixture fixture;
    SECTION("lightmapped fixed-point UVs") {
        put16(fixture.model, Fixture::kVertices + 12, 512);
        put16(fixture.model, Fixture::kVertices + 14, static_cast<u16>(-256));
    }
    SECTION("non-lightmapped floating-point UVs") {
        put32(fixture.model, Fixture::kLod + 4, 1);
        putFloat(fixture.model, Fixture::kVertices + 12, 0.5f);
        putFloat(fixture.model, Fixture::kVertices + 16, -0.25f);
    }
    const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
    CHECK(map.objects[1].mesh.vertices[0].uv == Vec2(32.0f, -8.0f));
}

TEST_CASE("Dreamcast Legends preview discloses unresolved borrowed texture slots",
          "[formats][legends]") {
    Fixture fixture;
    fixture.model[244 + 2] = 8;
    const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
    REQUIRE(map.warnings.size() == 2);
    CHECK(map.warnings.back().starts_with("1 borrowed texture slots"));
}

TEST_CASE("Dreamcast Legends shared vertices retain UVs across differently sized materials",
          "[formats][legends]") {
    Fixture fixture;
    // Insert a second bitmap record before the geometry and relocate the three
    // authored file offsets. The two disconnected triangles share vertices 1/2.
    fixture.model.insert(fixture.model.begin() + Fixture::kVertices, 80, 0);
    put32(fixture.model, 100, 2);
    put32(fixture.model, 88, Fixture::kDefinitions + 80);
    put32(fixture.model, Fixture::kLod + 12, Fixture::kVertices + 80);
    put32(fixture.model, Fixture::kLod + 20, Fixture::kTriangles + 80);
    put16(fixture.model, 324 + 4, 32);
    put16(fixture.model, 324 + 6, 16);
    put16(fixture.model, Fixture::kTriangles + 80 + 6, 0);
    put16(fixture.model, Fixture::kTriangles + 80 + 14, 1);
    put16(fixture.model, Fixture::kVertices + 80 + 24 + 12, 512);
    put16(fixture.model, Fixture::kVertices + 80 + 24 + 14, 256);
    const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
    const auto& mesh = map.objects[1].mesh;
    REQUIRE(mesh.parts.size() == 2);
    REQUIRE(mesh.vertices.size() == 6);
    CHECK(mesh.vertices[1].uv == Vec2(32.0f, 8.0f));
    // The second triangle has native reversed winding [3,2,1].
    const auto duplicate = mesh.parts[1].indices[2];
    CHECK(duplicate >= 4);
    CHECK(mesh.vertices[duplicate].position == mesh.vertices[1].position);
    CHECK(mesh.vertices[duplicate].uv == Vec2(16.0f, 4.0f));
}

TEST_CASE("Dreamcast Legends native map corpus is readable without conversion",
          "[formats][legends][assets]") {
    const auto levels =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::exists(levels)) {
        SKIP("Dreamcast Legends native extraction is not installed");
    }
    usize maps = 0;
    usize triangles = 0;
    usize collisionTriangles = 0;
    usize starts = 0;
    for (const auto& entry : std::filesystem::directory_iterator(levels)) {
        const auto world = entry.path() / "worlds.rom";
        const auto models = entry.path() / "objects.rom";
        if (!std::filesystem::exists(world) || !std::filesystem::exists(models) ||
            std::filesystem::file_size(world) == 0) {
            continue;
        }
        INFO(entry.path().string());
        const LegendsMap map = LegendsMap::parseDreamcast(readFile(world), readFile(models));
        REQUIRE_FALSE(map.objects.empty());
        REQUIRE_FALSE(map.collision.empty());
        collisionTriangles += map.collision.size();
        starts += map.starts.size();
        const bool noAuthoredStart =
            entry.path().filename() == "levela2" || entry.path().filename() == "levels6";
        CHECK(map.starts.empty() == noAuthoredStart);
        for (const auto& triangle : map.collision) {
            REQUIRE(triangle.owner >= 0);
            REQUIRE(static_cast<usize>(triangle.owner) < map.objects.size());
            CHECK(triangle.flags == map.objects[static_cast<usize>(triangle.owner)].flags);
            for (const auto& vertex : triangle.vertices) {
                REQUIRE(std::isfinite(vertex.x));
                REQUIRE(std::isfinite(vertex.y));
                REQUIRE(std::isfinite(vertex.z));
                CHECK(std::abs(glm::dot(vertex - triangle.vertices[0], triangle.normal)) < 0.01f);
            }
        }
        for (const auto& object : map.objects) {
            triangles += object.mesh.triangleCount();
            for (const auto& part : object.mesh.parts) {
                for (const u32 value : part.indices) {
                    REQUIRE(value < object.mesh.vertices.size());
                }
            }
        }
        ++maps;
    }
    CHECK(maps == 41);
    CHECK(triangles > 100000);
    CHECK(collisionTriangles == 61774);
    CHECK(starts == 44);
}

namespace {
Fixture collisionFixture() {
    Fixture fixture;
    fixture.world.resize(308);
    put32(fixture.world, 8, 1);
    put32(fixture.world, 12, 240);
    put16(fixture.world, Fixture::kMesh + 54, 1);
    put32(fixture.world, Fixture::kMesh + 56, 0);
    put32(fixture.world, Fixture::kMesh + 16, 6);
    putFloat(fixture.world, 244, std::numeric_limits<f32>::infinity());
    putFloat(fixture.world, 252, 1.0f);
    putFloat(fixture.world, 260, 4.0f);
    putFloat(fixture.world, 264, 5.0f);
    putFloat(fixture.world, 268, 6.0f);
    put16(fixture.world, 272, 128);
    put16(fixture.world, 278, 192);
    put32(fixture.world, 88, 1);
    put32(fixture.world, 92, 280);
    fixture.world[280] = 7;
    putFloat(fixture.world, 284, 40.0f);
    putFloat(fixture.world, 288, 50.0f);
    putFloat(fixture.world, 292, 60.0f);
    putFloat(fixture.world, 300, 1.25f);
    return fixture;
}
} // namespace

TEST_CASE("Dreamcast native collision is independent of render vertices and authored starts",
          "[formats][legends]") {
    auto fixture = collisionFixture();
    const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
    REQUIRE(map.collision.size() == 1);
    const auto& triangle = map.collision[0];
    CHECK(triangle.vertices[0] == Vec3(4, 5, 6));
    CHECK(triangle.vertices[1] == Vec3(6, 5, 6));
    CHECK(triangle.vertices[2] == Vec3(4, 5, 9));
    CHECK(triangle.normal == Vec3(0, 1, 0));
    CHECK(triangle.owner == 1);
    CHECK(triangle.nativeIndex == 0);
    CHECK(triangle.flags == 6);
    CHECK(triangle.floor);
    CHECK(triangle.wall);
    REQUIRE(map.starts.size() == 1);
    CHECK(map.starts[0].position == Vec3(40, 50, 60));
    CHECK(map.starts[0].facingRadians == 1.25f);
    CHECK(map.starts[0].nativeIndex == 0);
    CHECK(map.starts[0].flags == 7);
}

TEST_CASE("Dreamcast collision resolves native local dynamic coordinates and disables",
          "[formats][legends]") {
    auto fixture = collisionFixture();
    SECTION("dynamic local vertices") {
        put32(fixture.world, Fixture::kMesh + 16, 0x1006);
        const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
        CHECK(map.collision[0].vertices[0] == Vec3(15, 27, 39));
    }
    SECTION("dynamic child vertices") {
        put32(fixture.world, Fixture::kMesh + 16, 0x01003006);
        const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
        CHECK(map.collision[0].vertices[2] == Vec3(15, 27, 42));
    }
    SECTION("disabled object") {
        fixture.world[Fixture::kMesh + 53] = 1;
        const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
        CHECK_FALSE(map.collision[0].floor);
        CHECK_FALSE(map.collision[0].wall);
    }
    SECTION("disabled parent") {
        fixture.world[Fixture::kRecords + 53] = 1;
        const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
        CHECK_FALSE(map.collision[0].floor);
        CHECK_FALSE(map.collision[0].wall);
    }
    SECTION("unowned pool entries are not scenery solids") {
        put16(fixture.world, Fixture::kMesh + 54, 0);
        const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
        CHECK(map.collision.empty());
    }
    SECTION("camera locator is not a player start") {
        fixture.world[280] = 1;
        const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
        CHECK(map.starts.empty());
    }
    SECTION("Dreamcast locator kind is a full short") {
        fixture.world[281] = 1;
        const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
        CHECK(map.starts.empty());
    }
}

TEST_CASE("Dreamcast collision reconstructs native inclined and downward plane bases",
          "[formats][legends]") {
    auto fixture = collisionFixture();
    Vec3 expected;
    SECTION("downward pole") {
        putFloat(fixture.world, 252, -1.0f);
        expected = Vec3(4, 5, 3);
    }
    SECTION("wall plane") {
        putFloat(fixture.world, 244, 1.0f);
        putFloat(fixture.world, 248, 1.0f);
        putFloat(fixture.world, 252, 0.0f);
        expected = Vec3(4, 8, 6);
    }
    SECTION("inclined plane") {
        putFloat(fixture.world, 244, 1.25f);
        putFloat(fixture.world, 248, 0.8f);
        putFloat(fixture.world, 252, 0.6f);
        expected = Vec3(2.2f, 7.4f, 6);
    }
    const auto map = LegendsMap::parseDreamcast(fixture.world, fixture.model);
    CHECK(glm::length(map.collision[0].vertices[2] - expected) < 0.0001f);
}

TEST_CASE("Dreamcast collision and starts reject malformed native records", "[formats][legends]") {
    auto fixture = collisionFixture();
    SECTION("collision pool extent") {
        put32(fixture.world, 8, 0xffffffffU);
    }
    SECTION("collision owner extent") {
        put16(fixture.world, Fixture::kMesh + 54, 2);
    }
    SECTION("negative collision start") {
        put32(fixture.world, Fixture::kMesh + 56, 0xffffffffU);
    }
    SECTION("invalid plane") {
        putFloat(fixture.world, 252, 0.0f);
    }
    SECTION("non-finite origin") {
        putFloat(fixture.world, 260, std::numeric_limits<f32>::quiet_NaN());
    }
    SECTION("invalid reciprocal") {
        putFloat(fixture.world, 248, 1.0f);
        putFloat(fixture.world, 252, 0.0f);
    }
    SECTION("locator extent") {
        put32(fixture.world, 88, 2);
    }
    SECTION("non-finite start") {
        putFloat(fixture.world, 284, std::numeric_limits<f32>::quiet_NaN());
    }
    SECTION("non-finite facing") {
        putFloat(fixture.world, 300, std::numeric_limits<f32>::quiet_NaN());
    }
    CHECK_THROWS_AS(LegendsMap::parseDreamcast(fixture.world, fixture.model), FormatError);
}
