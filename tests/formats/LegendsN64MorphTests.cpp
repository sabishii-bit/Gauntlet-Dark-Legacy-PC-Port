#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/LegendsN64Morph.h"
#include "formats/LegendsN64Scene.h"

using namespace gdl;
using namespace gdl::formats;

namespace {

Mesh triangle() {
    Mesh result;
    result.vertices.resize(3);
    result.vertices[0].uv = {2, 3};
    result.vertices[1].color = Color::rgba(11, 22, 33, 77);
    result.parts.push_back({7, 0, {0, 1, 2}});
    return result;
}

} // namespace

TEST_CASE("N64 variant geometry budget counts all copied arrays without overflowing",
          "[formats][legends][morph][allocation]") {
    auto original = triangle();
    original.parts.push_back({});
    const usize one = sizeof(Mesh) + sizeof(std::vector<u32>) + 3 * sizeof(MeshVertex) +
                      2 * sizeof(MeshPart) + 3 * sizeof(u32) + 3 * sizeof(u32);
    CHECK(legendsN64VariantGeometryBytes(original, 3, 2, one * 2) == one * 2);
    CHECK_THROWS_AS(legendsN64VariantGeometryBytes(original, 3, 2, one * 2 - 1), FormatError);
    CHECK_THROWS_AS(legendsN64VariantGeometryBytes(original, 3, 0, one), FormatError);
    CHECK_THROWS_AS(legendsN64VariantGeometryBytes(original, std::numeric_limits<usize>::max(), 1,
                                                   kLegendsN64VariantByteLimit),
                    FormatError);
    CHECK_THROWS_AS(legendsN64VariantGeometryBytes(original, 3, std::numeric_limits<usize>::max(),
                                                   std::numeric_limits<usize>::max()),
                    FormatError);
    CHECK_THROWS_AS(legendsN64VariantGeometryBytes(original, 3, 1, 0), FormatError);
}

TEST_CASE("N64 morph rejects index-heavy amplification before copying any frames",
          "[formats][legends][morph][allocation]") {
    std::vector<u8> track(12 + 4096 * 3);
    track[0] = 0x10; // 4096 frames, one position key per frame.
    track[3] = 1;
    const std::array<u8, 4> binding{0, 1, 0, 0};
    const std::array<u32, 3> sources{0, 0, 0};
    auto original = triangle();
    original.parts[0].indices.resize(32768, 0); // 128KiB becomes512MiB across the frames.
    usize vertices = usize{4096} * 3;
    usize bytes = kLegendsN64VariantByteLimit;
    CHECK_THROWS_AS(decodeLegendsN64Morph(track, binding, original, sources, vertices, bytes),
                    FormatError);
    CHECK(vertices == usize{4096} * 3);
    CHECK(bytes == kLegendsN64VariantByteLimit);
}

TEST_CASE("N64 texture variants and morph frames spend the same archive geometry budget",
          "[formats][legends][morph][allocation]") {
    const std::vector<u8> track{0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::array<u8, 4> binding{0, 1, 0, 0};
    const std::array<u32, 3> sources{0, 0, 0};
    const auto original = triangle();
    const usize one =
        legendsN64VariantGeometryBytes(original, sources.size(), 1, kLegendsN64VariantByteLimit);
    usize bytes = one * 2;
    // Same charge used by Scene's runtime-texture variant path, leaving one frame.
    bytes -= legendsN64VariantGeometryBytes(original, sources.size(), 1, bytes);
    usize vertices = 6;
    REQUIRE(decodeLegendsN64Morph(track, binding, original, sources, vertices, bytes));
    CHECK(bytes == 0);
    CHECK(vertices == 3);
    CHECK_THROWS_AS(decodeLegendsN64Morph(track, binding, original, sources, vertices, bytes),
                    FormatError);
    CHECK(bytes == 0);
    CHECK(vertices == 3);
}

TEST_CASE("N64 morph preserves native vertex identity through duplicated cache uploads",
          "[formats][legends][morph]") {
    // Two absolute frames, two unique keys, XYZ base(-64,128,0), shifts(1,2,0).
    const std::vector<u8> track{0,  2,  0,  2,  0xff, 0xc0, 0, 0x80, 0, 0,   4,  0x40,
                                32, 16, 64, 64, 32,   128,  0, 0,    0, 128, 64, 255};
    const std::array<u8, 6> binding{0, 2, 0, 1, 0, 0};
    const std::array<u32, 3> sources{1, 0, 1};
    const auto original = triangle();
    usize budget = 6;
    usize byteBudget = kLegendsN64VariantByteLimit;
    const usize charge = legendsN64VariantGeometryBytes(original, sources.size(), 2, byteBudget);
    const auto decoded =
        decodeLegendsN64Morph(track, binding, original, sources, budget, byteBudget);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 2);
    REQUIRE(budget == 0);
    REQUIRE(byteBudget == kLegendsN64VariantByteLimit - charge);
    REQUIRE((*decoded)[0].vertices[0].position == Vec3(0, 3, 1));
    REQUIRE((*decoded)[0].vertices[1].position == Vec3(1, 4, 2));
    REQUIRE((*decoded)[0].vertices[2].position == (*decoded)[0].vertices[0].position);
    REQUIRE((*decoded)[1].vertices[0].position == Vec3(-1, 2, 0));
    REQUIRE((*decoded)[1].vertices[1].position == Vec3(3, 6, 255.0f / 64));
    for (const auto& mesh : *decoded) {
        REQUIRE(mesh.parts[0].texture == 7);
        REQUIRE(mesh.parts[0].indices == original.parts[0].indices);
        REQUIRE(mesh.vertices[0].uv == original.vertices[0].uv);
        REQUIRE(mesh.vertices[1].color == original.vertices[1].color);
        REQUIRE(mesh.vertices[0].normal == original.vertices[0].normal);
    }
}

TEST_CASE("N64 morph optional attributes retain alpha and obey normal versus color semantics",
          "[formats][legends][morph]") {
    const std::vector<u8> track{0x80, 1, 0, 1,  0,   0,   0,    0,    0,
                                0,    0, 0, 64, 128, 192, 0xff, 0x7f, 0x80};
    const std::array<u8, 4> binding{0, 1, 0, 0};
    const std::array<u32, 3> sources{0, 0, 0};
    auto original = triangle();
    usize budget = 6;
    usize byteBudget = kLegendsN64VariantByteLimit;
    const auto normals =
        decodeLegendsN64Morph(track, binding, original, sources, budget, byteBudget);
    REQUIRE(normals);
    const auto& normal = normals->front().vertices[1];
    REQUIRE(normal.position == Vec3(1, 2, 3));
    REQUIRE(normal.normal.x == Catch::Approx(-1.0f / 127));
    REQUIRE(normal.normal.y == 1);
    REQUIRE(normal.normal.z == Catch::Approx(-128.0f / 127));
    REQUIRE(normal.color == original.vertices[1].color);
    original.prelit = true;
    const auto colors =
        decodeLegendsN64Morph(track, binding, original, sources, budget, byteBudget);
    REQUIRE(colors);
    REQUIRE(colors->front().vertices[1].color == Color::rgba(255, 127, 128, 77));
    REQUIRE(colors->front().vertices[1].normal == original.vertices[1].normal);
}

TEST_CASE("N64 packed morphs accumulate from the original pose with signed16 wrapping",
          "[formats][legends][morph][barrels]") {
    // Two frames, one key, base(-4,-8,-12), shifts(1,2,3).
    // Packed XYZ=(3,4,5), then (1,2,3); packed RGB=(31,16,32), then zero.
    std::vector<u8> track{0x80, 2,    0,    1,    0xff, 0xfc, 0xff, 0xf8, 0xff, 0xf4,
                          0x84, 0x43, 0x14, 0x83, 0x82, 0x1f, 0x0c, 0x41, 0,    0};
    const std::array<u8, 4> binding{0, 1, 0, 0};
    const std::array<u32, 3> sources{0, 0, 0};
    auto original = triangle();
    original.prelit = true;
    original.vertices[0].position = {1, 2, 3};
    original.vertices[1].position = {32767.0f / 64, 0, 0};
    usize vertices = 6;
    usize bytes = kLegendsN64VariantByteLimit;
    const auto decoded = decodeLegendsN64Morph(track, binding, original, sources, vertices, bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 2);
    CHECK(decoded->front().vertices[0].position == Vec3(66, 136, 220) / 64.0f);
    CHECK(decoded->back().vertices[0].position == Vec3(64, 136, 232) / 64.0f);
    CHECK(decoded->front().vertices[1].position.x == -32767.0f / 64);
    CHECK(decoded->front().vertices[1].color == Color::rgba(248, 128, 128, 77));
    CHECK(decoded->back().vertices[1].color == Color::rgba(0, 0, 0, 77));
    CHECK(decoded->back().parts[0].indices == original.parts[0].indices);
    CHECK(decoded->back().vertices[0].uv == original.vertices[0].uv);
    CHECK(vertices == 0);
    track.pop_back();
    vertices = 6;
    CHECK_THROWS_AS(decodeLegendsN64Morph(track, binding, original, sources, vertices, bytes),
                    FormatError);
    CHECK(vertices == 6);
}

TEST_CASE("Native barrel break animation decodes every accumulated vertex frame",
          "[formats][legends][morph][barrels][native]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/items/c/barrel";
    if (!std::filesystem::is_regular_file(directory / "seq.wad")) {
        SKIP("Native N64 Legends barrel archive unavailable");
    }
    const auto sequence = readFile(directory / "seq.wad");
    const auto models = decodeLegendsN64Models(readFile(directory / "objects.wad"));
    REQUIRE(models.size() == 3);
    const auto& model = models[2];
    REQUIRE(model.name == "BAR_BROKEOANIM");
    usize vertices = 1000000;
    usize bytes = kLegendsN64VariantByteLimit;
    const auto frames = decodeLegendsN64Morph(std::span{sequence}.subspan(0xd8),
                                              std::span{sequence}.subspan(8, 172), model.mesh,
                                              model.vertexSources, vertices, bytes);
    REQUIRE(frames);
    REQUIRE(frames->size() == 23);
    CHECK(frames->front().vertices != (*frames)[10].vertices);
    CHECK(frames->back().vertices != (*frames)[10].vertices);
    const auto decoded = decodeLegendsN64Sequences(sequence, models);
    CHECK(std::ranges::none_of(decoded.warnings, [](const auto& warning) {
        return warning.starts_with("CONTAINER:CBRA:");
    }));
    const auto tree =
        std::ranges::find(decoded.animation.trees, std::string_view("CONTAINER:CBRA"), &TreeDefinition::name);
    REQUIRE(tree != decoded.animation.trees.end());
    REQUIRE(tree->nodes.front().objectFrames.size() == 1);
    CHECK(tree->nodes.front().objectFrames.front().frames == 23);
    // Decoding again begins from the immutable model, never the last rendered frame.
    vertices = 1000000;
    bytes = kLegendsN64VariantByteLimit;
    const auto again = decodeLegendsN64Morph(std::span{sequence}.subspan(0xd8),
                                             std::span{sequence}.subspan(8, 172), model.mesh,
                                             model.vertexSources, vertices, bytes);
    REQUIRE(again);
    CHECK(again->front().vertices == frames->front().vertices);
}

TEST_CASE("N64 morph validates extents mappings and allocation before producing geometry",
          "[formats][legends][morph]") {
    std::vector<u8> track{0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::array<u8, 4> binding{0, 1, 0, 0};
    const std::array<u32, 3> sources{0, 0, 0};
    const auto original = triangle();
    usize budget = 3;
    usize byteBudget = kLegendsN64VariantByteLimit;
    REQUIRE_THROWS_AS(decodeLegendsN64Morph({}, binding, original, sources, budget, byteBudget),
                      FormatError);
    REQUIRE_THROWS_AS(decodeLegendsN64Morph(track, {}, original, sources, budget, byteBudget),
                      FormatError);
    REQUIRE_THROWS_AS(decodeLegendsN64Morph(track, binding, original, {}, budget, byteBudget),
                      FormatError);
    const std::array<u8, 4> badBinding{0, 1, 0, 1};
    REQUIRE_THROWS_AS(
        decodeLegendsN64Morph(track, badBinding, original, sources, budget, byteBudget),
        FormatError);
    const std::array<u32, 3> badSources{0, 1, 0};
    REQUIRE_THROWS_AS(
        decodeLegendsN64Morph(track, binding, original, badSources, budget, byteBudget),
        FormatError);
    track.pop_back();
    REQUIRE_THROWS_AS(decodeLegendsN64Morph(track, binding, original, sources, budget, byteBudget),
                      FormatError);
    track.push_back(0);
    REQUIRE(budget == 3);
    budget = 2;
    REQUIRE_THROWS_AS(decodeLegendsN64Morph(track, binding, original, sources, budget, byteBudget),
                      FormatError);
    REQUIRE(budget == 2);
    track[10] = 0x80;
    REQUIRE_THROWS_AS(decodeLegendsN64Morph(track, binding, original, sources, budget, byteBudget),
                      FormatError);
    REQUIRE(budget == 2);
    REQUIRE(byteBudget == kLegendsN64VariantByteLimit);
}

TEST_CASE("Native explosive barrel breakup decodes packed positions without replacing normals",
          "[formats][legends][morph][barrels][native]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted/items/c";
    if (!std::filesystem::is_regular_file(root / "redbar/seq.wad")) {
        SKIP("Native N64 Legends explosive barrels unavailable");
    }
    for (const char* const bank : {"redbar", "grebar"}) {
        CAPTURE(bank);
        const auto sequence = readFile(root / bank / "seq.wad");
        const auto models = decodeLegendsN64Models(readFile(root / bank / "objects.wad"));
        REQUIRE(models.size() == 3);
        const auto& model = models[2];
        REQUIRE_FALSE(model.mesh.prelit);
        usize vertices = 1000000;
        usize bytes = kLegendsN64VariantByteLimit;
        const auto frames = decodeLegendsN64Morph(std::span{sequence}.subspan(0x120),
                                                  std::span{sequence}.subspan(8, 236), model.mesh,
                                                  model.vertexSources, vertices, bytes);
        REQUIRE(frames);
        REQUIRE(frames->size() == 27);
        CHECK(frames->front().vertices != frames->back().vertices);
        for (const auto& frame : *frames) {
            REQUIRE(frame.vertices.size() == model.mesh.vertices.size());
            for (usize i = 0; i < frame.vertices.size(); ++i) {
                CHECK(frame.vertices[i].normal == model.mesh.vertices[i].normal);
                CHECK(frame.vertices[i].uv == model.mesh.vertices[i].uv);
                CHECK(frame.vertices[i].color == model.mesh.vertices[i].color);
            }
        }
        const auto decoded = decodeLegendsN64Sequences(sequence, models);
        const char* const wanted = std::string_view(bank) == "redbar" ? "TRAP:TBRA" : "TRAP:TBGA";
        const auto tree = std::ranges::find(decoded.animation.trees, wanted, &TreeDefinition::name);
        REQUIRE(tree != decoded.animation.trees.end());
        REQUIRE(tree->nodes.front().objectFrames.size() == 1);
        CHECK(tree->nodes.front().objectFrames.front().frames == 27);
        CHECK(std::ranges::none_of(
            decoded.warnings, [&](const auto& warning) { return warning.starts_with(wanted); }));
    }
}

TEST_CASE("Native N64 podium page uses absolute vertex keyframes and the authored permutation",
          "[formats][legends][morph][native]") {
    const auto directory = test::dataDirectory().parent_path() / "assets/n64/extracted/items/tower";
    if (!std::filesystem::exists(directory / "seq.wad")) {
        SKIP("Native N64 Legends tower archive unavailable");
    }
    const auto sequence = readFile(directory / "seq.wad");
    const auto models = decodeLegendsN64Models(readFile(directory / "objects.wad"));
    const auto page = std::find_if(models.begin(), models.end(),
                                   [](const auto& model) { return model.name == "PODIUMOANIM"; });
    REQUIRE(page != models.end());
    REQUIRE(page->vertexSources.size() == page->mesh.vertices.size());
    const auto sourceZero = std::find(page->vertexSources.begin(), page->vertexSources.end(), 0);
    REQUIRE(sourceZero != page->vertexSources.end());
    const usize zero = static_cast<usize>(sourceZero - page->vertexSources.begin());
    // Authored BODY node52 points to BDAT offset8, whose first raw vertex uses
    // key6. FLIP key6 is (59,16,63)+(-49,225,56); REDY is (7,4,12)+(3,237,107).
    const auto binding = std::span{sequence}.subspan(8, 34);
    usize budget = 10000;
    usize byteBudget = kLegendsN64VariantByteLimit;
    const auto flip = decodeLegendsN64Morph(std::span{sequence}.subspan(0x580), binding, page->mesh,
                                            page->vertexSources, budget, byteBudget);
    const auto ready = decodeLegendsN64Morph(std::span{sequence}.subspan(0xad8), binding,
                                             page->mesh, page->vertexSources, budget, byteBudget);
    REQUIRE(flip);
    REQUIRE(ready);
    REQUIRE(flip->size() == 14);
    REQUIRE(ready->size() == 1);
    REQUIRE(flip->front().vertices[zero].position == Vec3(10, 241, 119) / 64.0f);
    REQUIRE(ready->front().vertices[zero].position == flip->front().vertices[zero].position);
    REQUIRE((*flip)[6].vertices != flip->front().vertices);
    for (const auto& frame : *flip) {
        REQUIRE(frame.parts[0].indices == page->mesh.parts[0].indices);
        REQUIRE(frame.vertices[zero].uv == page->mesh.vertices[zero].uv);
    }
}

TEST_CASE("N64 property16 absolute keys retain the same native vertex mapping as morphs",
          "[formats][legends][morph][portal]") {
    // Header: keep-before,2 frames,no delay,2 keys,base(-64,128,0),shifts(1,2,0).
    const std::vector<u8> track{4, 0,    0,  2,  0,  0,  0,  2,   0xff, 0xc0, 0, 0x80, 0,  0,
                                4, 0x40, 32, 16, 64, 64, 32, 128, 0,    0,    0, 128,  64, 255};
    const std::array<u8, 6> binding{0, 2, 0, 1, 0, 0};
    const std::array<u32, 3> sources{1, 0, 1};
    const auto original = triangle();
    usize vertices = 6;
    usize bytes = kLegendsN64VariantByteLimit;
    const auto decoded =
        decodeLegendsN64VertexKeys(track, binding, original, sources, vertices, bytes);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 2);
    CHECK(vertices == 0);
    CHECK((*decoded)[0].vertices[0].position == Vec3(0, 3, 1));
    CHECK((*decoded)[0].vertices[1].position == Vec3(1, 4, 2));
    CHECK((*decoded)[0].vertices[2].position == (*decoded)[0].vertices[0].position);
    CHECK((*decoded)[1].vertices[0].position == Vec3(-1, 2, 0));
    CHECK((*decoded)[1].vertices[1].position == Vec3(3, 6, 255.0f / 64));
    REQUIRE((*decoded)[1].parts.size() == original.parts.size());
    CHECK((*decoded)[1].parts[0].indices == original.parts[0].indices);
    CHECK((*decoded)[1].parts[0].texture == original.parts[0].texture);
    CHECK((*decoded)[1].vertices[1].color == original.vertices[1].color);
    CHECK((*decoded)[1].vertices[0].uv == original.vertices[0].uv);
}

TEST_CASE("N64 property16 rejects unsupported timing and malformed inputs without spending budgets",
          "[formats][legends][morph][portal]") {
    std::vector<u8> track{4, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::array<u8, 4> binding{0, 1, 0, 0};
    const std::array<u32, 3> sources{0, 0, 0};
    const auto original = triangle();
    usize vertices = 3;
    usize bytes = kLegendsN64VariantByteLimit;
    SECTION("packed delta") {
        track[0] |= 1;
        CHECK_FALSE(decodeLegendsN64VertexKeys(track, binding, original, sources, vertices, bytes));
    }
    SECTION("delayed start") {
        track[5] = 1;
        CHECK_FALSE(decodeLegendsN64VertexKeys(track, binding, original, sources, vertices, bytes));
    }
    SECTION("truncated header") {
        track.resize(15);
        CHECK_THROWS_AS(
            decodeLegendsN64VertexKeys(track, binding, original, sources, vertices, bytes),
            FormatError);
    }
    SECTION("truncated frame") {
        track.pop_back();
        CHECK_THROWS_AS(
            decodeLegendsN64VertexKeys(track, binding, original, sources, vertices, bytes),
            FormatError);
    }
    SECTION("invalid vertex key") {
        const std::array<u8, 4> invalid{0, 1, 0, 1};
        CHECK_THROWS_AS(
            decodeLegendsN64VertexKeys(track, invalid, original, sources, vertices, bytes),
            FormatError);
    }
    SECTION("allocation budget") {
        const usize one = legendsN64VariantGeometryBytes(original, sources.size(), 1, bytes);
        bytes = one - 1;
        CHECK_THROWS_AS(
            decodeLegendsN64VertexKeys(track, binding, original, sources, vertices, bytes),
            FormatError);
        CHECK(bytes == one - 1);
        bytes = kLegendsN64VariantByteLimit;
    }
    CHECK(vertices == 3);
    CHECK(bytes == kLegendsN64VariantByteLimit);
}

TEST_CASE("Native N64 exits raise and collapse their authored transport surface",
          "[formats][legends][morph][portal][native]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "itemanim/hires";
    if (!std::filesystem::is_regular_file(directory / "seq.wad")) {
        SKIP("Native N64 Legends exit artwork is not installed");
    }
    const auto models = decodeLegendsN64Models(readFile(directory / "objects.wad"));
    const auto decoded = decodeLegendsN64Sequences(readFile(directory / "seq.wad"), models,
                                                   readFile(root / "_program/game.bin"));
    const auto tree = [&](std::string_view sequence) -> const TreeDefinition& {
        const auto found = std::ranges::find(
            decoded.animation.trees, legendsN64TreeName("TRAP", sequence), &TreeDefinition::name);
        REQUIRE(found != decoded.animation.trees.end());
        return *found;
    };
    const auto mesh = [&](const TreeDefinition& definition, usize frame) -> const Mesh& {
        REQUIRE(definition.nodes.size() > 20);
        const auto& surface = definition.nodes[20];
        REQUIRE(surface.objectFrames.size() == 1);
        const auto& run = surface.objectFrames[0];
        const auto first =
            std::ranges::find(decoded.modelVariants, run.object, &LegendsN64Model::name);
        REQUIRE(first != decoded.modelVariants.end());
        REQUIRE(frame < static_cast<usize>(run.frames));
        REQUIRE(frame < static_cast<usize>(decoded.modelVariants.end() - first));
        return (first + static_cast<std::ptrdiff_t>(frame))->mesh;
    };
    const auto& starting = tree("EXNS");
    const auto& ending = tree("EXNE");
    const auto& idle = tree("EXNR");
    const auto& active = tree("EXNA");
    REQUIRE(starting.nodes.size() > 20);
    REQUIRE(ending.nodes.size() > 20);
    REQUIRE(starting.nodes[20].objectFrames.size() == 1);
    REQUIRE(ending.nodes[20].objectFrames.size() == 1);
    CHECK(starting.nodes[20].objectFrames[0].frames == 15);
    CHECK(ending.nodes[20].objectFrames[0].frames == 15);
    CHECK((ending.sequences[0].flags & 1U) != 0); // common effectFrame reverses the run
    const auto maximumY = [](const Mesh& value) {
        f32 height = -std::numeric_limits<f32>::infinity();
        for (const auto& vertex : value.vertices) {
            height = std::max(height, vertex.position.y);
        }
        return height;
    };
    CHECK(maximumY(mesh(idle, 0)) < .1f);
    CHECK(maximumY(mesh(starting, 0)) < .1f);
    CHECK(maximumY(mesh(starting, 7)) > 1);
    CHECK(maximumY(mesh(starting, 14)) > 6);
    CHECK(maximumY(mesh(active, 0)) > 6);
    for (usize frame = 0; frame < 15; ++frame) {
        CHECK(mesh(starting, frame).vertices == mesh(ending, frame).vertices);
    }
    for (const auto& warning : decoded.warnings) {
        CHECK_FALSE(warning.starts_with("TRAP:EXN"));
    }
}
