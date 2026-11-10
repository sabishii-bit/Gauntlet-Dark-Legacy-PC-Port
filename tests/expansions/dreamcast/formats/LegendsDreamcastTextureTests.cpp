#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"

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

struct Fixture {
    static constexpr usize kBitmap = 104;
    static constexpr usize kPixels = 8;
    std::vector<u8> models = std::vector<u8>(184);
    std::vector<u8> pixels;

    Fixture(u16 width, u16 height, u8 kind, u8 format, usize size) : pixels(size + kPixels) {
        put32(models, 64, 0xf00b0001U);
        put32(models, 100, 1);
        put16(models, kBitmap, 255);
        put16(models, kBitmap + 4, width);
        put16(models, kBitmap + 6, height);
        put32(models, kBitmap + 8, kPixels);
        models[kBitmap + 16] = format;
        models[kBitmap + 17] = kind;
        put32(models, kBitmap + 20, static_cast<u32>(size));
    }

    LegendsMapTexture decode() const {
        return decodeLegendsDreamcastTextures(models, pixels).at(0);
    }
};
} // namespace

TEST_CASE("Dreamcast Legends direct colors preserve native RGB and alpha",
          "[formats][legends][texture]") {
    SECTION("ARGB1555") {
        Fixture fixture(2, 1, 9, 0, 4);
        put16(fixture.pixels, Fixture::kPixels, 0xfc00);
        put16(fixture.pixels, Fixture::kPixels + 2, 0x03e0);
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(0, 255, 0, 0));
        CHECK(texture.hasTransparency);
    }
    SECTION("RGB565") {
        Fixture fixture(2, 1, 9, 1, 4);
        put16(fixture.pixels, Fixture::kPixels, 0x07e0);
        put16(fixture.pixels, Fixture::kPixels + 2, 0x001f);
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(0, 255, 0, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(0, 0, 255, 255));
        CHECK_FALSE(texture.hasTransparency);
    }
    SECTION("ARGB4444") {
        Fixture fixture(1, 1, 9, 2, 2);
        put16(fixture.pixels, Fixture::kPixels, 0x8123);
        fixture.models[Fixture::kBitmap + 2] = 3;
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(17, 34, 51, 136));
        CHECK(texture.clampU);
        CHECK(texture.clampV);
    }
}

TEST_CASE("Dreamcast lightmaps decode independent RGB565 descriptors and reject corrupt data",
          "[formats][legends][dreamcast-presentation]") {
    std::vector<u8> records(12);
    put32(records, 4, 0x901);
    put16(records, 8, 2);
    put16(records, 10, 1);
    std::vector<u8> pixels(4);
    put16(pixels, 0, 0xf800);
    put16(pixels, 2, 0x07e0);
    const auto image = decodeDreamcastLightmap(records, pixels, 0);
    CHECK(image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
    CHECK(image.pixel(1, 0) == Color::rgba(0, 255, 0, 255));
    SECTION("descriptor extent") {
        records.resize(11);
    }
    SECTION("pixel extent") {
        pixels.resize(3);
    }
    SECTION("pixel offset") {
        put32(records, 0, 0xffffffff);
    }
    SECTION("dimensions") {
        put16(records, 8, 3);
    }
    SECTION("encoding") {
        put32(records, 4, 0xff01);
    }
    CHECK_THROWS_AS(decodeDreamcastLightmap(records, pixels, 0), FormatError);
}

TEST_CASE("Dreamcast Legends twiddle addresses interleave Y then X",
          "[formats][legends][texture]") {
    Fixture fixture(4, 2, 13, 1, 16);
    put16(fixture.pixels, Fixture::kPixels + 2, 0xf800);  // (0,1)
    put16(fixture.pixels, Fixture::kPixels + 4, 0x07e0);  // (1,0)
    put16(fixture.pixels, Fixture::kPixels + 12, 0x001f); // (3,0)
    const auto texture = fixture.decode();
    CHECK(texture.image.pixel(0, 1) == Color::rgba(255, 0, 0, 255));
    CHECK(texture.image.pixel(1, 0) == Color::rgba(0, 255, 0, 255));
    CHECK(texture.image.pixel(3, 0) == Color::rgba(0, 0, 255, 255));
}

TEST_CASE("Dreamcast Legends native mip chains skip file rather than hardware padding",
          "[formats][legends][texture]") {
    Fixture fixture(8, 8, 2, 1, 172);
    put16(fixture.pixels, Fixture::kPixels + 44, 0xf800);
    put16(fixture.pixels, Fixture::kPixels + 46, 0x07e0);
    const auto texture = fixture.decode();
    CHECK(texture.image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
    CHECK(texture.image.pixel(0, 1) == Color::rgba(0, 255, 0, 255));
}

TEST_CASE("Dreamcast Legends VQ codebook and mip variants preserve 2x2 ordering",
          "[formats][legends][texture]") {
    u16 width = 8;
    u8 kind = 3;
    usize offset = 2048;
    SECTION("full VQ") {}
    SECTION("full VQ mip") {
        kind = 4;
        offset += 6;
    }
    SECTION("compact VQ") {
        kind = 16;
        offset = 128;
    }
    SECTION("compact VQ mip") {
        kind = 17;
        offset = 134;
    }
    SECTION("32px compact VQ uses 32 entries") {
        width = 32;
        kind = 16;
        offset = 256;
    }
    SECTION("32px compact mip uses 64 entries") {
        width = 32;
        kind = 17;
        offset = 512 + 86;
    }
    SECTION("64px compact mip uses 256 entries") {
        width = 64;
        kind = 17;
        offset = 2048 + 342;
    }
    Fixture fixture(width, width, kind, 1, offset + usize{width} * width / 4);
    put16(fixture.pixels, Fixture::kPixels + 8, 0xf800);
    put16(fixture.pixels, Fixture::kPixels + 10, 0x07e0);
    put16(fixture.pixels, Fixture::kPixels + 12, 0x001f);
    put16(fixture.pixels, Fixture::kPixels + 14, 0xffff);
    fixture.pixels[Fixture::kPixels + offset] = 1;
    const auto texture = fixture.decode();
    CHECK(texture.image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
    CHECK(texture.image.pixel(0, 1) == Color::rgba(0, 255, 0, 255));
    CHECK(texture.image.pixel(1, 0) == Color::rgba(0, 0, 255, 255));
    CHECK(texture.image.pixel(1, 1) == Color::rgba(255, 255, 255, 255));
}

TEST_CASE("Dreamcast generator textures decode their full detail native VQ mip",
          "[formats][legends][texture][dreamcast-presentation][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/generators";
    if (!std::filesystem::is_regular_file(directory / "objects.rom")) {
        SKIP("Optional Dreamcast assets are not installed");
    }
    const auto textures = decodeLegendsDreamcastTextures(readFile(directory / "objects.rom"),
                                                         readFile(directory / "textures.rom"));
    struct Reference {
        std::string_view name;
        u32 size;
        u64 hash;
    };
    // Independent PVR VRAM reconstruction: kmLoadTexture 8c0321f4 copies
    // table 8c057724's compact codebook to 0 and the index chain to 0x800.
    // Table 8c057798 gives the complete chain length. Decode its last N*N/4
    // indices in Morton order (Y then X), then hash row-major RGBA with FNV1a.
    // This catches accidentally reading a lower mip with unchanged dimensions.
    constexpr std::array kExpected{Reference{"GEN_TRO1_BRKN3_2", 16, 0x2307e6df7865c2edULL},
                                   Reference{"GEN_TRO1_CAVE_2", 16, 0x7180f813ba0e6e2bULL},
                                   Reference{"GEN_TRO1_FRONT_2", 16, 0x1aa826420dcde5b3ULL},
                                   Reference{"GEN_TRO1_TOP_2", 16, 0xcbceb84cb20b1ee6ULL},
                                   Reference{"GEN_TRO3_FILE_10", 32, 0x1d5692bd00a0d44fULL}};
    for (const auto& reference : kExpected) {
        INFO(reference.name);
        const auto found = std::ranges::find(textures, reference.name, &LegendsMapTexture::name);
        REQUIRE(found != textures.end());
        CHECK(found->image.width == reference.size);
        CHECK(found->image.height == reference.size);
        u64 hash = 0xcbf29ce484222325ULL;
        for (const u8 byte : found->image.pixels) {
            hash = (hash ^ byte) * 0x100000001b3ULL;
        }
        CHECK(hash == reference.hash);
    }
}

TEST_CASE("Dreamcast Legends borrowed textures keep their slots without inventing pixels",
          "[formats][legends][texture]") {
    Fixture fixture(16, 32, 2, 1, 0);
    fixture.models[Fixture::kBitmap + 2] = 9;
    put32(fixture.models, Fixture::kBitmap + 8, 0xffffffffU);
    const auto texture = fixture.decode();
    CHECK(texture.image.pixels.empty());
    CHECK(texture.external);
    CHECK(texture.width == 16);
    CHECK(texture.height == 32);
    CHECK(texture.clampU);
    CHECK_FALSE(texture.clampV);
}

TEST_CASE("Dreamcast bitmap names retain native slot bindings and aliases",
          "[formats][legends][texture]") {
    Fixture fixture(2, 1, 9, 1, 4);
    put32(fixture.models, 68, 2);
    put32(fixture.models, 92, 184);
    fixture.models.resize(184 + 72);
    const std::string first = "FLAME_00";
    const std::string alias = "FLAME_ALIAS";
    std::copy(first.begin(), first.end(), fixture.models.begin() + 184);
    std::copy(alias.begin(), alias.end(), fixture.models.begin() + 220);
    const auto texture = fixture.decode();
    CHECK(texture.name == first);
    CHECK_FALSE(texture.external);
    CHECK(texture.width == 2);
    CHECK(texture.height == 1);
    put16(fixture.models, 220 + 30, 1);
    CHECK_THROWS_AS(fixture.decode(), FormatError);
}

TEST_CASE("Dreamcast Legends textures reject malformed records and payloads",
          "[formats][legends][texture]") {
    Fixture fixture(8, 8, 16, 1, 144);
    SECTION("short header") {
        fixture.models.resize(103);
    }
    SECTION("wrong version") {
        put32(fixture.models, 64, 0);
    }
    SECTION("model count overflow") {
        put32(fixture.models, 96, 0xffffffffU);
    }
    SECTION("bitmap count overflow") {
        put32(fixture.models, 100, 0xffffffffU);
    }
    SECTION("short bitmap") {
        fixture.models.resize(183);
    }
    SECTION("signature") {
        put16(fixture.models, Fixture::kBitmap, 0);
    }
    SECTION("zero width") {
        put16(fixture.models, Fixture::kBitmap + 4, 0);
    }
    SECTION("non-power-of-two height") {
        put16(fixture.models, Fixture::kBitmap + 6, 3);
    }
    SECTION("huge width") {
        put16(fixture.models, Fixture::kBitmap + 4, 2048);
    }
    SECTION("pixel offset") {
        put32(fixture.models, Fixture::kBitmap + 8, 0xfffffff0U);
    }
    SECTION("pixel extent") {
        put32(fixture.models, Fixture::kBitmap + 20, 145);
    }
    SECTION("pixel encoding") {
        fixture.models[Fixture::kBitmap + 16] = 7;
    }
    SECTION("storage encoding") {
        fixture.models[Fixture::kBitmap + 17] = 99;
    }
    SECTION("non-square VQ") {
        put16(fixture.models, Fixture::kBitmap + 6, 16);
    }
    SECTION("truncated codebook") {
        put32(fixture.models, Fixture::kBitmap + 20, 127);
    }
    SECTION("truncated indices") {
        put32(fixture.models, Fixture::kBitmap + 20, 143);
    }
    SECTION("compact index out of range") {
        fixture.pixels[Fixture::kPixels + 128] = 16;
    }
    CHECK_THROWS_AS(fixture.decode(), FormatError);
}

TEST_CASE("all extracted Dreamcast level texture archives decode natively",
          "[formats][legends][texture][assets]") {
    const auto root =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_directory(root)) {
        SKIP("native Dreamcast extraction is not installed");
    }
    usize maps = 0;
    usize decoded = 0;
    usize borrowed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto world = entry.path() / "worlds.rom";
        const auto models = entry.path() / "objects.rom";
        const auto pixels = entry.path() / "textures.rom";
        if (!std::filesystem::is_regular_file(world) || std::filesystem::file_size(world) == 0 ||
            !std::filesystem::is_regular_file(models) ||
            !std::filesystem::is_regular_file(pixels)) {
            continue;
        }
        INFO(entry.path().string());
        const auto textures = decodeLegendsDreamcastTextures(readFile(models), readFile(pixels));
        REQUIRE_FALSE(textures.empty());
        for (const auto& texture : textures) {
            if (texture.image.pixels.empty()) {
                ++borrowed;
            } else {
                CHECK(texture.image.pixels.size() ==
                      usize{texture.image.width} * texture.image.height * 4);
                ++decoded;
            }
        }
        ++maps;
    }
    CHECK(maps == 41);
    CHECK(decoded > 1000);
    CHECK(borrowed > 0);
}
