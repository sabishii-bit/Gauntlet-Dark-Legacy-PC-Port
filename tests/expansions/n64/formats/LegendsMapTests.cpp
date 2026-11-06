#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/shared/formats/LegendsMap.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8U);
    bytes.at(at + 1) = static_cast<u8>(value);
}

void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}

void putFloat(std::vector<u8>& bytes, usize at, f32 value) {
    put32(bytes, at, std::bit_cast<u32>(value));
}

struct Section {
    std::string_view tag;
    u32 offset;
    u32 count;
};

void directory(std::vector<u8>& bytes, std::initializer_list<Section> sections) {
    const usize start = bytes.size();
    bytes.resize(start + sections.size() * 12);
    put32(bytes, 0, static_cast<u32>(start));
    put32(bytes, 4, static_cast<u32>(sections.size()));
    usize at = start;
    for (const Section& section : sections) {
        for (usize i = 0; i < 4; ++i) {
            bytes[at + i] = static_cast<u8>(section.tag[i]);
        }
        put32(bytes, at + 4, section.offset);
        put32(bytes, at + 8, section.count);
        at += 12;
    }
}

struct Fixture {
    static constexpr usize kWorldHeader = 8;
    static constexpr usize kMatrix = 44;
    static constexpr usize kWorldName = 92;
    static constexpr usize kPlacement = 100;
    static constexpr usize kObjectHeader = 40;
    static constexpr usize kVertices = 96;
    static constexpr usize kList = 144;
    std::vector<u8> world = std::vector<u8>(144);
    std::vector<u8> model = std::vector<u8>(176);

    Fixture() {
        for (usize axis = 0; axis < 3; ++axis) {
            putFloat(world, kWorldHeader + axis * 4, -10.0f);
            putFloat(world, kWorldHeader + 12 + axis * 4, 20.0f);
            putFloat(world, kMatrix + axis * 16, 1.0f);
        }
        putFloat(world, kMatrix + 36, 5.0f);
        for (usize i = 0; i < 4; ++i) {
            world[kWorldName + i] = static_cast<u8>("ROCK"[i]);
            model[8 + i] = static_cast<u8>("ROCK"[i]);
        }
        put32(world, kPlacement, 0x100);
        put32(world, kPlacement + 4, 0x09001800);
        put16(world, kPlacement + 36, 0xffff);
        put16(world, kPlacement + 38, 0xffff);
        directory(world, {{"WHDR", kWorldHeader, 36},
                          {"MTRX", kMatrix, 1},
                          {"WONM", kWorldName, 5},
                          {"WOBJ", kPlacement, 1}});
        put32(model, 16 + 4, 0x1234); // Texture-load list identity; survey does not execute it.
        put32(model, kObjectHeader + 8, 4);
        put32(model, kObjectHeader + 12, kList);
        put32(model, kObjectHeader + 16, kVertices);
        for (usize i = 0; i < 3; ++i) {
            put16(model, kVertices + i * 16, static_cast<u16>(i * 64));
            put16(model, kVertices + i * 16 + 8, static_cast<u16>(i * 32));
            model[kVertices + i * 16 + 12] = 255;
            model[kVertices + i * 16 + 15] = 128;
        }
        put32(model, kList, 0x01003006); // Three vertices into cache slots 0..2.
        put32(model, kList + 4, 0x0e000000);
        put32(model, kList + 8, 0xde000000);
        put32(model, kList + 12, 0x1234);
        put32(model, kList + 16, 0x05000204);
        put32(model, kList + 24, 0xdf000000);
        directory(
            model,
            {{"NAME", 8, 5}, {"TXHD", 16, 1}, {"OBHD", kObjectHeader, 1}, {"OBJS", kVertices, 80}});
    }
};

struct CollisionFixture : Fixture {
    static constexpr usize kTriangles = 144;
    static constexpr usize kCollisionVertices = 168;
    static constexpr usize kGrid = 198;
    static constexpr usize kLists = 204;
    static constexpr usize kDirectory = 212;
    std::vector<u8> markers = std::vector<u8>(108);

    CollisionFixture() {
        world.resize(kDirectory);
        const std::array<Vec3, 5> vertices{{{0, 0, 0}, {4, 0, 0}, {0, 0, 4}, {0, 4, 0}, {4, 4, 0}}};
        for (usize i = 0; i < vertices.size(); ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                put16(world, kCollisionVertices + i * 6 + static_cast<usize>(axis) * 2,
                      static_cast<u16>(vertices[i][axis] * 32));
            }
        }
        const std::array<std::array<u16, 4>, 3> triangles{
            {{0, 2, 1, 0x10}, {0, 1, 3, 0x20}, {0, 2, 4, 0x4010}}};
        for (usize i = 0; i < triangles.size(); ++i) {
            for (usize field = 0; field < 4; ++field) {
                put16(world, kTriangles + i * 8 + field * 2, triangles[i][field]);
            }
        }
        put16(world, kGrid, 0);
        put16(world, kGrid + 2, 2);
        put16(world, kGrid + 4, 0xffff);
        put16(world, kLists, 0);
        put16(world, kLists + 2, 0x8001);
        put16(world, kLists + 4, 0xf000);
        put16(world, kLists + 6, 0x8002);
        directory(world, {{"WHDR", kWorldHeader, 36},
                          {"MTRX", kMatrix, 1},
                          {"WONM", kWorldName, 5},
                          {"WOBJ", kPlacement, 1},
                          {"CFOT", kTriangles, 32},
                          {"CTRI", kTriangles, 3},
                          {"CVTX", kCollisionVertices, 5},
                          {"GRID", kGrid, 3},
                          {"CLST", kLists, 8}});
        put32(markers, 20, 92);
        put16(markers, 48, 2);
        markers[98] = 0x25; // Paired path marker, not a player entrance.
        put16(markers, 100, static_cast<u16>(-8));
        put16(markers, 102, 12);
        put16(markers, 104, 20);
        markers[106] = 0x20;
        markers[107] = 7;
    }
};
} // namespace

TEST_CASE("N64 model part names retain the native validated OBHD suffix",
          "[legends][n64][formats][native-traps]") {
    Fixture fixture;
    fixture.model[Fixture::kObjectHeader + 6] = 2;
    auto models = decodeLegendsN64Models(fixture.model);
    REQUIRE(models.size() == 1);
    CHECK(models.front().name == "ROCK");
    CHECK(models.front().partName == "CK");
    fixture.model[Fixture::kObjectHeader + 6] = 0;
    CHECK(decodeLegendsN64Models(fixture.model).front().partName == "ROCK");
    fixture.model[Fixture::kObjectHeader + 6] = 5;
    CHECK_THROWS_AS(decodeLegendsN64Models(fixture.model), FormatError);
    fixture.model[Fixture::kObjectHeader + 6] = 0xff;
    CHECK_THROWS_AS(decodeLegendsN64Models(fixture.model), FormatError);
    // Renamed retail weapon models sometimes keep a prefix reaching the next
    // NAME string. The native accessor reads that bounded alias unchanged.
    put32(fixture.model, 184, 7); // NAME count in this fixture's directory.
    fixture.model[13] = 'G';
    fixture.model[14] = 0;
    fixture.model[Fixture::kObjectHeader + 6] = 5;
    CHECK(decodeLegendsN64Models(fixture.model).front().partName == "G");
}

TEST_CASE("N64 Legends maps decode native placements and F3DEX2 geometry", "[formats][legends]") {
    const Fixture fixture;
    const LegendsMap map = LegendsMap::parseN64(fixture.world, fixture.model);
    REQUIRE(map.objects.size() == 1);
    const auto& object = map.objects.front();
    CHECK(object.name == "ROCK");
    CHECK(object.flags == 0x100);
    CHECK(object.nativeDrawFlags == 0x09001800);
    CHECK(object.parent == -1);
    CHECK(object.transform[3][0] == 5.0f);
    CHECK(object.mesh.flags == 0); // Native N64 flags must not become GameCube material bits.
    REQUIRE(object.mesh.vertices.size() == 3);
    CHECK(object.mesh.vertices[2].position == Vec3{2.0f, 0.0f, 0.0f});
    CHECK(object.mesh.vertices[2].uv == Vec2{2.0f, 0.0f});
    CHECK(object.mesh.vertices[2].color == Color::rgba(255, 0, 0, 128));
    REQUIRE(object.mesh.parts.size() == 1);
    CHECK(object.mesh.parts[0].indices == std::vector<u32>{0, 1, 2});
    CHECK(object.partCullBack == std::vector<bool>{true});
    CHECK_FALSE(map.warnings.empty());
}

TEST_CASE("Native world culling follows triangle-time geometry mode without merging overrides",
          "[formats][legends][native-world-culling]") {
    Fixture fixture;
    const usize at = Fixture::kList;
    fixture.model.resize(at + 64);
    put32(fixture.model, at + 24, 0xd9fff9ff); // Clear both cull bits.
    put32(fixture.model, at + 28, 0);
    put32(fixture.model, at + 32, 0x05000204);
    put32(fixture.model, at + 36, 0);
    put32(fixture.model, at + 40, 0xd9ffffff); // Restore native back-face culling.
    put32(fixture.model, at + 44, 0x400);
    put32(fixture.model, at + 48, 0x05000204);
    put32(fixture.model, at + 52, 0);
    put32(fixture.model, at + 56, 0xdf000000);
    directory(fixture.model, {{"NAME", 8, 5},
                              {"TXHD", 16, 1},
                              {"OBHD", Fixture::kObjectHeader, 1},
                              {"OBJS", Fixture::kVertices, 112}});
    const auto map = LegendsMap::parseN64(fixture.world, fixture.model);
    REQUIRE(map.objects.size() == 1);
    const auto& object = map.objects.front();
    REQUIRE(object.mesh.parts.size() == 3);
    CHECK(object.partCullBack == std::vector<bool>{true, false, true});
    CHECK(object.mesh.triangleCount() == 3);
    for (const auto& part : object.mesh.parts) {
        CHECK(part.indices == std::vector<u32>{0, 1, 2});
    }
    // Unimplemented front-cull semantics must never silently become back-cull.
    put32(fixture.model, at + 44, 0x200);
    CHECK_THROWS_AS(LegendsMap::parseN64(fixture.world, fixture.model), FormatError);
}

TEST_CASE("N64 model color ramps retain authored combiner endpoints per draw part",
          "[formats][legends][color-ramp]") {
    Fixture fixture;
    fixture.model.resize(Fixture::kList + 64);
    const usize at = Fixture::kList;
    put32(fixture.model, at + 16, 0xfc50ffff);
    put32(fixture.model, at + 20, 0x3ffdf238);
    put32(fixture.model, at + 24, 0xfa000000);
    put32(fixture.model, at + 28, 0x801000ff);
    put32(fixture.model, at + 32, 0xfb000000);
    put32(fixture.model, at + 36, 0xffff00ff);
    put32(fixture.model, at + 40, 0x05000204);
    put32(fixture.model, at + 44, 0);
    put32(fixture.model, at + 48, 0xfc000000); // Other combiners must not inherit this ramp.
    put32(fixture.model, at + 52, 0);
    put32(fixture.model, at + 56, 0x05000204);
    put32(fixture.model, at + 60, 0);
    fixture.model.resize(at + 72);
    put32(fixture.model, at + 64, 0xdf000000);
    directory(fixture.model, {{"NAME", 8, 5},
                              {"TXHD", 16, 1},
                              {"OBHD", Fixture::kObjectHeader, 1},
                              {"OBJS", Fixture::kVertices, 120}});
    const auto models = decodeLegendsN64Models(fixture.model);
    REQUIRE(models.size() == 1);
    const auto& model = models.front();
    REQUIRE(model.colorRamps.size() == 1);
    CHECK(model.colorRamps.front().part == 0);
    CHECK(model.colorRamps.front().primitive == Color::rgba(128, 16, 0, 255));
    CHECK(model.colorRamps.front().environment == Color::rgba(255, 255, 0, 255));
    REQUIRE(model.mesh.parts.size() == 2);
    CHECK(model.mesh.parts[0].texture == model.mesh.parts[1].texture);
    CHECK(model.mesh.parts[0].indices == model.mesh.parts[1].indices);
    CHECK(model.mesh.vertices[0].color == Color::rgba(255, 0, 0, 128));
    put32(fixture.model, at + 24, 0xe7000000); // Recognized ramp with no primitive color.
    CHECK_THROWS_AS(decodeLegendsN64Models(fixture.model), FormatError);
}

TEST_CASE("N64 world hierarchy retains parents without moving collision owners",
          "[formats][legends][motion]") {
    Fixture fixture;
    constexpr usize kCount = 4;
    fixture.world.resize(Fixture::kPlacement + kCount * 44);
    for (usize i = 1; i < kCount; ++i) {
        std::copy_n(fixture.world.begin() + Fixture::kPlacement, 44,
                    fixture.world.begin() +
                        static_cast<std::ptrdiff_t>(Fixture::kPlacement + i * 44));
    }
    // Node0 has child1, node1 has child3 and sibling2. This deliberately differs
    // from numeric index order: native lookup visits 0,1,3,2.
    put16(fixture.world, Fixture::kPlacement + 38, 1);
    put16(fixture.world, Fixture::kPlacement + 44 + 36, 2);
    put16(fixture.world, Fixture::kPlacement + 44 + 38, 3);
    directory(fixture.world, {{"WHDR", Fixture::kWorldHeader, 36},
                              {"MTRX", Fixture::kMatrix, 1},
                              {"WONM", Fixture::kWorldName, 5},
                              {"WOBJ", Fixture::kPlacement, kCount}});
    const auto map = LegendsMap::parseN64(fixture.world, fixture.model);
    REQUIRE(map.objects.size() == kCount);
    CHECK(map.objects[0].parent == -1);
    CHECK(map.objects[1].parent == 0);
    CHECK(map.objects[2].parent == 0);
    CHECK(map.objects[3].parent == 1);
    CHECK(map.objects[0].nativeOrder == 0);
    CHECK(map.objects[1].nativeOrder == 1);
    CHECK(map.objects[2].nativeOrder == 3);
    CHECK(map.objects[3].nativeOrder == 2);
    for (const auto& object : map.objects) {
        CHECK(object.transform[3].x == 5); // No guessed parent transform composition.
    }

    SECTION("self-cycle") {
        put16(fixture.world, Fixture::kPlacement + 38, 0);
    }
    SECTION("shared child") {
        put16(fixture.world, Fixture::kPlacement + usize{2} * 44 + 38, 3);
    }
    SECTION("out of bounds child") {
        put16(fixture.world, Fixture::kPlacement + 38, kCount);
    }
    SECTION("sibling cycle") {
        put16(fixture.world, Fixture::kPlacement + usize{2} * 44 + 36, 1);
    }
    SECTION("unreachable records") {
        put16(fixture.world, Fixture::kPlacement + 38, 0xffff);
    }
    CHECK_THROWS_AS(LegendsMap::parseN64(fixture.world, fixture.model), FormatError);
}

TEST_CASE("Native texture-only combiners ignore shade without corrupting later shaded draws",
          "[legends][formats][native-effect-material]") {
    Fixture fixture;
    const usize at = Fixture::kList;
    fixture.model.resize(at + 72);
    put32(fixture.model, at + 16, 0xe2000e0e);
    put32(fixture.model, at + 20, 0x4240);
    put32(fixture.model, at + 24, 0xfcffffff);
    put32(fixture.model, at + 28, 0xfffcf238);
    put32(fixture.model, at + 32, 0x05000204);
    put32(fixture.model, at + 40, 0xe2000e0e);
    put32(fixture.model, at + 44, 0x4240);
    put32(fixture.model, at + 48, 0xfcfffe04); // SHADE*TEXEL0, unlike DECALRGBA.
    put32(fixture.model, at + 52, 0xfffcf3f8);
    put32(fixture.model, at + 56, 0x05000204);
    put32(fixture.model, at + 64, 0xdf000000);
    directory(fixture.model, {{"NAME", 8, 5},
                              {"TXHD", 16, 1},
                              {"OBHD", Fixture::kObjectHeader, 1},
                              {"OBJS", Fixture::kVertices, 120}});
    auto models = decodeLegendsN64Models(fixture.model);
    REQUIRE(models.size() == 1);
    const auto& model = models.front();
    REQUIRE(model.mesh.parts.size() == 1);
    REQUIRE(model.mesh.parts[0].indices.size() == 6);
    CHECK(model.mesh.vertices.size() == 6);
    for (usize i = 0; i < 3; ++i) {
        const auto first = model.mesh.parts[0].indices[i];
        const auto second = model.mesh.parts[0].indices[i + 3];
        CHECK(model.mesh.vertices[first].color == Color::white());
        CHECK(model.mesh.vertices[second].color == Color::rgba(255, 0, 0, 128));
        CHECK(model.mesh.vertices[first].position == model.mesh.vertices[second].position);
        CHECK(model.vertexSources[first] == model.vertexSources[second]);
    }
    CHECK_FALSE(model.depthFlags); // Shaded/mixed-combiner materials keep their existing policy.
    CHECK_FALSE(model.mixedDepthFlags);
    put32(fixture.model, at + 48, 0xfcffffff);
    put32(fixture.model, at + 52, 0xfffcf238);
    models = decodeLegendsN64Models(fixture.model);
    CHECK(models.front().depthFlags == (object_flags::kNoDepthTest | object_flags::kNoDepthWrite));
    CHECK(models.front().mesh.vertices.size() == 3);
    put32(fixture.model, at + 44, 0x4a70); // The second part now restores compare AND write.
    models = decodeLegendsN64Models(fixture.model);
    CHECK_FALSE(models.front().depthFlags);
    CHECK(models.front().mixedDepthFlags); // Never flatten changing state into one node policy.
    put32(fixture.model, at + 20, 0x4a70);
    models = decodeLegendsN64Models(fixture.model);
    REQUIRE(models.front().depthFlags);
    CHECK(*models.front().depthFlags == 0);
    CHECK_FALSE(models.front().mixedDepthFlags);
}

TEST_CASE("N64 LOD lighting flags distinguish signed normals from prelit RGB",
          "[formats][legends]") {
    Fixture fixture;
    auto models = decodeLegendsN64Models(fixture.model);
    REQUIRE(models.size() == 1);
    REQUIRE(models[0].mesh.prelit);
    CHECK(models[0].mesh.vertices[0].color == Color::rgba(255, 0, 0, 128));
    put32(fixture.model, Fixture::kObjectHeader + 8, 2);
    fixture.model[Fixture::kVertices + 12] = 129; // -127
    fixture.model[Fixture::kVertices + 13] = 0;
    fixture.model[Fixture::kVertices + 14] = 127;
    models = decodeLegendsN64Models(fixture.model);
    REQUIRE_FALSE(models[0].mesh.prelit);
    CHECK(models[0].mesh.vertices[0].normal == Vec3{-1, 0, 1});
    CHECK(models[0].mesh.vertices[0].color == Color::rgba(255, 255, 255, 128));
}

TEST_CASE("N64 segment13 materials remain explicit until a drawable binds them",
          "[formats][legends][n64]") {
    Fixture fixture;
    put32(fixture.model, Fixture::kList + 12, 0x0d000000);
    const auto models = decodeLegendsN64Models(fixture.model);
    REQUIRE(models.size() == 1);
    REQUIRE(models[0].mesh.parts.size() == 1);
    CHECK(models[0].vertexSources == std::vector<u32>{0, 1, 2});
    CHECK(models[0].mesh.parts[0].texture == LegendsN64Model::kRuntimeTexture);
    CHECK(models[0].mesh.parts[0].indices == std::vector<u32>{0, 1, 2});
    // World geometry has no per-drawable animation binding; don't accept the
    // runtime segment or arbitrary offsets there as a local texture identity.
    CHECK_THROWS_AS(LegendsMap::parseN64(fixture.world, fixture.model), FormatError);
    put32(fixture.model, Fixture::kList + 12, 0x0d000008);
    CHECK_THROWS_AS(decodeLegendsN64Models(fixture.model), FormatError);
}

TEST_CASE("N64 Legends geometry rejects malformed native data", "[formats][legends]") {
    Fixture fixture;
    SECTION("truncated header") {
        fixture.world.resize(7);
    }
    SECTION("oversized directory") {
        put32(fixture.world, 4, 0xffffffffU);
    }
    SECTION("directory inside header") {
        put32(fixture.world, 0, 0);
    }
    SECTION("oversized section") {
        put32(fixture.world, 144 + 8, 10000);
    }
    SECTION("duplicate section") {
        for (usize i = 0; i < 4; ++i) {
            fixture.world[144 + 12 + i] = fixture.world[144 + i];
        }
    }
    SECTION("bad placement matrix") {
        put16(fixture.world, Fixture::kPlacement + 10, 1);
    }
    SECTION("non-finite matrix") {
        putFloat(fixture.world, Fixture::kMatrix, std::numeric_limits<f32>::infinity());
    }
    SECTION("unverified rotated placement") {
        putFloat(fixture.world, Fixture::kMatrix, -1.0f);
    }
    SECTION("unterminated name") {
        fixture.world[Fixture::kWorldName + 4] = 'X';
    }
    SECTION("display list points into header") {
        put32(fixture.model, Fixture::kObjectHeader + 12, 8);
    }
    SECTION("vertex pool truncated") {
        put32(fixture.model, Fixture::kList + 4, 0x0e000040);
    }
    SECTION("invalid vertex segment") {
        put32(fixture.model, Fixture::kList + 4, 0x02000000);
    }
    SECTION("invalid vertex cache range") {
        put32(fixture.model, Fixture::kList, 0x01003002);
    }
    SECTION("unloaded triangle vertex") {
        put32(fixture.model, Fixture::kList + 16, 0x05000206);
    }
    SECTION("odd triangle index") {
        put32(fixture.model, Fixture::kList + 16, 0x05000203);
    }
    SECTION("unsupported display list call") {
        put32(fixture.model, Fixture::kList + 12, 0x09000000);
    }
    SECTION("cross-archive texture call") {
        put32(fixture.model, Fixture::kList + 12, 0x08000400);
    }
    SECTION("display-list branch is not a material call") {
        put32(fixture.model, Fixture::kList + 8, 0xde010000);
    }
    SECTION("unsupported opcode") {
        put32(fixture.model, Fixture::kList + 24, 0xff000000);
    }
    SECTION("unterminated list") {
        put32(fixture.model, Fixture::kList + 24, 0xe7000000);
    }
    CHECK_THROWS_AS(LegendsMap::parseN64(fixture.world, fixture.model), FormatError);
}

TEST_CASE("N64 models resolve local texture segments without executing arbitrary lists",
          "[formats][legends][n64][texture-segment]") {
    Fixture fixture;
    fixture.model.resize(184);
    put32(fixture.model, Fixture::kList, 0xdb060004);
    put32(fixture.model, Fixture::kList + 4, 0x1234);
    put32(fixture.model, Fixture::kList + 8, 0x01003006);
    put32(fixture.model, Fixture::kList + 12, 0x0e000000);
    put32(fixture.model, Fixture::kList + 16, 0xde000000);
    put32(fixture.model, Fixture::kList + 20, 0x01000000);
    put32(fixture.model, Fixture::kList + 24, 0x05000204);
    put32(fixture.model, Fixture::kList + 28, 0);
    put32(fixture.model, Fixture::kList + 32, 0xdf000000);
    directory(fixture.model, {{"NAME", 8, 5},
                              {"TXHD", 16, 1},
                              {"OBHD", Fixture::kObjectHeader, 1},
                              {"OBJS", Fixture::kVertices, 88}});
    const auto models = decodeLegendsN64Models(fixture.model);
    REQUIRE(models.size() == 1);
    REQUIRE(models[0].mesh.parts.size() == 1);
    CHECK(models[0].mesh.parts[0].texture == 0);
    CHECK(models[0].mesh.parts[0].indices == std::vector<u32>{0, 1, 2});

    SECTION("unknown segment address") {
        put32(fixture.model, Fixture::kList + 4, Fixture::kList);
    }
    SECTION("unaligned segment index") {
        put32(fixture.model, Fixture::kList, 0xdb060005);
    }
    SECTION("unhandled MOVEWORD is not silently ignored") {
        put32(fixture.model, Fixture::kList, 0xdb020000);
    }
    SECTION("drawable vertex segment cannot be rebound as a texture") {
        put32(fixture.model, Fixture::kList, 0xdb060038);
    }
    SECTION("segment call needs a binding") {
        put32(fixture.model, Fixture::kList + 20, 0x02000000);
    }
    SECTION("call cannot enter the middle of a texture list") {
        put32(fixture.model, Fixture::kList + 20, 0x01000008);
    }
    CHECK_THROWS_AS(decodeLegendsN64Models(fixture.model), FormatError);
}

TEST_CASE("N64 Legends collision and entrances retain native data independently of meshes",
          "[formats][legends][legends-collision]") {
    CollisionFixture fixture;
    const LegendsMap map = LegendsMap::parseN64(fixture.world, fixture.model, fixture.markers);
    REQUIRE(map.collision.size() == 3);
    const auto& ground = map.collision[0];
    CHECK(ground.vertices[1] == Vec3{0, 0, 4});
    CHECK(ground.normal == Vec3{0, 1, 0});
    CHECK(ground.floor);
    CHECK_FALSE(ground.wall);
    CHECK(ground.owner == -1);
    CHECK(ground.flags == 0x10);
    CHECK(map.collision[1].wall);
    CHECK_FALSE(map.collision[1].floor);
    CHECK(map.collision[1].normal == Vec3{0, 0, 1});
    CHECK(map.collision[2].owner == 0);
    CHECK(map.collision[2].nativeIndex == 2);
    CHECK(map.collision[2].flags == 0x4010);
    CHECK(map.collision[2].normal.y == Catch::Approx(std::sqrt(0.5f)));
    // The visible model has X=5 placement; CVTX is already world-space.
    CHECK(map.collision[2].vertices[0] == Vec3{0});
    REQUIRE(map.starts.size() == 1);
    CHECK(map.starts[0].position == Vec3{-2, 3, 5});
    CHECK(map.starts[0].nativeIndex == 1);
    CHECK(map.starts[0].flags == 7);
    CHECK(map.starts[0].facingRadians == 0);

    SECTION("CFOT is directory metadata and its absence defaults to 64") {
        fixture.world[CollisionFixture::kDirectory + usize{4} * 12] = 'X';
        const LegendsMap changed = LegendsMap::parseN64(fixture.world, fixture.model);
        CHECK(changed.collision[0].vertices[1] == Vec3{0, 0, 2});
    }
    SECTION("inactive owner preserves raw triangles without making them walkable") {
        put32(fixture.world, Fixture::kPlacement, 0x40000);
        const LegendsMap changed = LegendsMap::parseN64(fixture.world, fixture.model);
        CHECK(changed.collision[2].owner == 0);
        CHECK_FALSE(changed.collision[2].floor);
        CHECK_FALSE(changed.collision[2].wall);
        CHECK(changed.collision[0].floor);
    }
}

TEST_CASE("N64 Legends collision rejects invalid indices and unterminated ownership lists",
          "[formats][legends][legends-collision]") {
    CollisionFixture fixture;
    SECTION("invalid fixed point") {
        put32(fixture.world, CollisionFixture::kDirectory + usize{4} * 12 + 8, 0);
    }
    SECTION("collision vertex outside pool") {
        put16(fixture.world, CollisionFixture::kTriangles, 5);
    }
    SECTION("collision triangle outside pool") {
        put16(fixture.world, CollisionFixture::kLists, 3);
    }
    SECTION("unknown owner") {
        put16(fixture.world, CollisionFixture::kLists + 4, 0xf001);
    }
    SECTION("conflicting owners") {
        put16(fixture.world, CollisionFixture::kLists + 6, 0x8000);
    }
    SECTION("missing cell terminator") {
        put16(fixture.world, CollisionFixture::kLists + 6, 2);
    }
    SECTION("invalid cell offset") {
        put16(fixture.world, CollisionFixture::kGrid, 0xfffe);
    }
    SECTION("truncated cell offset") {
        put16(fixture.world, CollisionFixture::kGrid, 4);
    }
    SECTION("degenerate triangle") {
        put16(fixture.world, CollisionFixture::kTriangles + 2, 0);
    }
    SECTION("truncated markers") {
        fixture.markers.resize(107);
    }
    SECTION("markers overlap header") {
        put32(fixture.markers, 20, 12);
    }
    CHECK_THROWS_AS(LegendsMap::parseN64(fixture.world, fixture.model, fixture.markers),
                    FormatError);
}

TEST_CASE("N64 Legends native map corpus is readable without conversion",
          "[formats][legends][assets]") {
    const std::filesystem::path levels =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels";
    if (!std::filesystem::exists(levels)) {
        SKIP("N64 Legends native extraction is not installed");
    }
    usize maps = 0;
    usize triangles = 0;
    usize collisionTriangles = 0;
    usize starts = 0;
    for (const auto& entry : std::filesystem::directory_iterator(levels)) {
        const auto world = entry.path() / "worlds.wad";
        const auto models = entry.path() / "objects.wad";
        if (!std::filesystem::exists(world) || !std::filesystem::exists(models)) {
            continue;
        }
        INFO(entry.path().string());
        const LegendsMap map = LegendsMap::parseN64(readFile(world), readFile(models),
                                                    readFile(entry.path() / "ipr.rom"));
        REQUIRE_FALSE(map.objects.empty());
        REQUIRE_FALSE(map.collision.empty());
        REQUIRE(map.starts.size() == 1);
        if (entry.path().filename() == "tower2") {
            for (const auto& [name, flags] : std::array<std::pair<std::string_view, u32>, 3>{
                     {{"NSFF_FLAME_00C14", 0x09001800},
                      {"XSNS_SNOWFALL__NC01", 0x08001800},
                      {"NS_LAVAFLOW__NC", 0x08001000}}}) {
                CAPTURE(name);
                const auto found =
                    std::find_if(map.objects.begin(), map.objects.end(),
                                 [&](const auto& object) { return object.name == name; });
                REQUIRE(found != map.objects.end());
                CHECK(found->nativeDrawFlags == flags);
            }
        }
        starts += map.starts.size();
        collisionTriangles += map.collision.size();
        for (const auto& triangle : map.collision) {
            CHECK(glm::dot(triangle.normal, triangle.normal) == Catch::Approx(1.0f));
            CHECK(triangle.owner >= -1);
            CHECK(triangle.owner < static_cast<s32>(map.objects.size()));
            for (int axis = 0; axis < 3; ++axis) {
                for (const auto& vertex : triangle.vertices) {
                    // Native collision is independently authored, but shares world units.
                    CHECK(vertex[axis] >= map.minBounds[axis] - 1.0f);
                    CHECK(vertex[axis] <= map.maxBounds[axis] + 1.0f);
                }
            }
        }
        for (const auto& object : map.objects) {
            triangles += object.mesh.triangleCount();
            for (const auto& part : object.mesh.parts) {
                for (const u32 index : part.indices) {
                    REQUIRE(index < object.mesh.vertices.size());
                }
            }
        }
        ++maps;
    }
    CHECK(maps == 34);
    CHECK(triangles > 50000);
    CHECK(collisionTriangles == 61502);
    CHECK(starts == 34);
}
