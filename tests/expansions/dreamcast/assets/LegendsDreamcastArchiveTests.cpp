#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value);
    bytes.at(at + 1) = static_cast<u8>(value >> 8U);
}

void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value));
    put16(bytes, at + 2, static_cast<u16>(value >> 16U));
}

/** A v1 objects.rom holding one model, QUAD: two strips over four vertices with
 * fixed-point texel and lightmap coordinates, one 64x32 rectangular RGB565 bitmap
 * and the 2x1 lightmap descriptor its strips name, with the pixels both read. */
struct Library {
    static constexpr usize kLod = 116;
    static constexpr usize kBitmap = 244;
    static constexpr usize kVertices = 324;
    static constexpr usize kTriangles = 420;
    static constexpr usize kDefinitions = 436;
    static constexpr usize kLightmap = 460;
    static constexpr usize kSecondLightmap = 472;
    static constexpr u32 kBitmapBytes = 64 * 32 * 2;
    static constexpr u16 kRed = 0xf800;
    static constexpr u16 kGreen = 0x07e0;
    std::vector<u8> models = std::vector<u8>(472);
    std::vector<u8> pixels = std::vector<u8>(kBitmapBytes + 4);

    Library() {
        put32(models, 64, 0xf00b0001U);
        put32(models, 88, kDefinitions);
        put32(models, 96, 1);
        put32(models, 100, 1);
        put16(models, kBitmap, 0xff);
        put16(models, kBitmap + 4, 64);
        put16(models, kBitmap + 6, 32);
        models[kBitmap + 16] = 1; // RGB565
        models[kBitmap + 17] = 9; // rectangular
        put32(models, kBitmap + 20, kBitmapBytes);
        put32(models, kLod + 4, 16); // fixed-point texel and lightmap coordinates
        put32(models, kLod + 8, 4);
        put32(models, kLod + 12, kVertices);
        put32(models, kLod + 16, 2);
        put32(models, kLod + 20, kTriangles);
        put32(models, kLod + 28, kLightmap);
        const std::array<Vec3, 4> vertices{{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}}};
        for (usize i = 0; i < vertices.size(); ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                put32(models, kVertices + i * 24 + static_cast<usize>(axis) * 4,
                      std::bit_cast<u32>(vertices[i][axis]));
            }
        }
        // The first vertex sits half way across and a quarter down both its
        // bitmap and its lightmap, in the native 1/1024 fixed-point units.
        put16(models, kVertices + 12, 512);
        put16(models, kVertices + 14, 256);
        put16(models, kVertices + 16, 512);
        put16(models, kVertices + 18, 256);
        // The second is half way across and down its lightmap.
        put16(models, kVertices + 24 + 16, 512);
        put16(models, kVertices + 24 + 18, 512);
        put16(models, kTriangles, 1);
        put16(models, kTriangles + 2, 2);
        put16(models, kTriangles + 4, 0);
        put16(models, kTriangles + 6, 0xc000);
        put16(models, kTriangles + 8, 3);
        put16(models, kTriangles + 10, 1);
        put16(models, kTriangles + 12, 2);
        constexpr std::string_view kName = "QUAD";
        std::ranges::copy(kName, models.begin() + kDefinitions);
        lightmap(kLightmap, kBitmapBytes, 2, 1);
        put16(pixels, kBitmapBytes, kRed);
        put16(pixels, kBitmapBytes + 2, kGreen);
    }

    void lightmap(usize record, u32 offset, u16 width, u16 height) {
        put32(models, record, offset);
        models.at(record + 4) = 1; // RGB565
        models.at(record + 5) = 9; // rectangular
        put16(models, record + 8, width);
        put16(models, record + 10, height);
    }

    /** Gives each strip a lightmap of its own: the second a 4x2 descriptor at 472. */
    void splitLightmaps() {
        models.resize(kSecondLightmap + 12);
        put16(models, kTriangles + 6, 0x4000);
        put16(models, kTriangles + 14, 0x4000);
        lightmap(kSecondLightmap, kBitmapBytes + 4, 4, 2);
        pixels.resize(kBitmapBytes + 4 + 4 * 2 * 2);
    }

    std::filesystem::path write(std::string_view name, bool withPixels = true) const {
        const auto root = test::scratchDirectory(name);
        writeFile(root / "objects.rom", models);
        if (withPixels) {
            writeFile(root / "textures.rom", pixels);
        }
        return root;
    }
};

const MeshVertex* vertexAt(const Mesh& mesh, Vec3 position) {
    const auto found = std::ranges::find(mesh.vertices, position, &MeshVertex::position);
    return found == mesh.vertices.end() ? nullptr : &*found;
}
} // namespace

TEST_CASE("Dreamcast archives without a model library are absent and malformed ones refused",
          "[legends][dreamcast-archive]") {
    const auto empty = test::scratchDirectory("dreamcast-archive-absent");
    CHECK_FALSE(loadLegendsDreamcastArchive(empty));
    CHECK_FALSE(loadLegendsDreamcastArchive(empty / "missing"));
    const auto truncated = test::scratchDirectory("dreamcast-archive-truncated");
    writeFile(truncated / "objects.rom", std::vector<u8>(8, 0xff));
    CHECK_THROWS_AS(loadLegendsDreamcastArchive(truncated), FormatError);
    // A bitmap that is not borrowed needs its pixels.
    const Library library;
    CHECK_THROWS_AS(
        loadLegendsDreamcastArchive(library.write("dreamcast-archive-pixelless", false)),
        FormatError);
}

TEST_CASE("Dreamcast archives normalize texel coordinates and bind each lightmap once",
          "[legends][dreamcast-archive]") {
    const Library library;
    const auto archive = loadLegendsDreamcastArchive(library.write("dreamcast-archive-lightmap"));
    REQUIRE(archive);
    REQUIRE(archive->models.size() == 1);
    CHECK(archive->models.entry(0).name == "QUAD");
    const auto& mesh = archive->models.mesh(0);
    CHECK(archive->models.entry(0).flags == mesh.flags);
    // The bitmap, then the lightmap its strips name, appended once and clamped.
    REQUIRE(archive->textures.size() == 2);
    CHECK(archive->textures.entry(0).width == 64);
    CHECK(archive->textures.entry(0).height == 32);
    CHECK_FALSE(archive->textures.entry(0).clampU);
    const auto& lightmap = archive->textures.entry(1);
    CHECK(lightmap.name == "__LIGHTMAP_460");
    CHECK(lightmap.width == 2);
    CHECK(lightmap.height == 1);
    CHECK(lightmap.clampU);
    CHECK(lightmap.clampV);
    CHECK(archive->textures.image(1).pixel(0, 0) == Color::rgba(255, 0, 0, 255));
    CHECK(archive->textures.image(1).pixel(1, 0) == Color::rgba(0, 255, 0, 255));
    REQUIRE(mesh.parts.size() == 1);
    CHECK(mesh.parts[0].texture == 0);
    CHECK(mesh.parts[0].lightmap == 1);
    CHECK(mesh.parts[0].rgbLightmap);
    CHECK(mesh.vertices.size() == 4);
    // Bitmap coordinates come out over the bitmap's size; lightmap ones in its texels.
    const auto* corner = vertexAt(mesh, {0, 0, 0});
    REQUIRE(corner);
    CHECK(corner->uv == Vec2(.5f, .25f));
    CHECK(corner->lightmapUv == Vec2(1, .25f));
    // Without an anim.rom the archive has no trees.
    CHECK(archive->trees.size() == 0);
}

TEST_CASE("Dreamcast archive vertices shared by two lightmaps keep a coordinate for each",
          "[legends][dreamcast-archive]") {
    Library library;
    library.splitLightmaps();
    const auto archive = loadLegendsDreamcastArchive(library.write("dreamcast-archive-split"));
    REQUIRE(archive);
    REQUIRE(archive->textures.size() == 3);
    CHECK(archive->textures.entry(1).name == "__LIGHTMAP_460");
    CHECK(archive->textures.entry(2).name == "__LIGHTMAP_472");
    const auto& mesh = archive->models.mesh(0);
    REQUIRE(mesh.parts.size() == 2);
    CHECK(mesh.parts[0].lightmap == 1);
    CHECK(mesh.parts[1].lightmap == 2);
    // The two strips share vertices 1 and 2, which each lightmap now holds apart.
    CHECK(mesh.vertices.size() == 6);
    std::vector<Vec2> shared;
    for (const auto& vertex : mesh.vertices) {
        if (vertex.position == Vec3{1, 0, 0}) {
            shared.push_back(vertex.lightmapUv);
        }
    }
    std::ranges::sort(shared, [](Vec2 a, Vec2 b) { return a.x < b.x; });
    CHECK(shared == std::vector<Vec2>{{1, .5f}, {2, 1}});
}

TEST_CASE("Dreamcast archives that borrow every bitmap need no pixel file",
          "[legends][dreamcast-archive]") {
    Library library;
    library.models[Library::kBitmap + 2] = 8; // borrowed from another archive
    // Plain floating-point coordinates, which name no lightmap descriptor.
    put32(library.models, Library::kLod + 4, 1);
    put32(library.models, Library::kLod + 28, 0);
    const auto archive =
        loadLegendsDreamcastArchive(library.write("dreamcast-archive-borrowed", false));
    REQUIRE(archive);
    REQUIRE(archive->textures.size() == 1);
    CHECK(archive->textures.entry(0).external());
    CHECK_FALSE(archive->textures.entry(0).noPicture);
    const auto& mesh = archive->models.mesh(0);
    CHECK(std::ranges::none_of(mesh.parts, &MeshPart::rgbLightmap));
}
