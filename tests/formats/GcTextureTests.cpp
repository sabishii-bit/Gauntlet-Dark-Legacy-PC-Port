#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/GcTexture.h"
#include "formats/ModelArchive.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

void putBigU16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value >> 8U));
    out.push_back(static_cast<u8>(value & 0xFFU));
}

TEST_CASE("RGB5A3 texels expand to eight-bit colour", "[formats][texture]") {
    REQUIRE(rgb5a3ToColor(0xFFFF) == Color::rgba(255, 255, 255, 255));
    REQUIRE(rgb5a3ToColor(0x8000) == Color::rgba(0, 0, 0, 255));
    REQUIRE(rgb5a3ToColor(0xFC00) == Color::rgba(255, 0, 0, 255));
    REQUIRE(rgb5a3ToColor(0x0000) == Color::rgba(0, 0, 0, 0));
    REQUIRE(rgb5a3ToColor(0x7FFF) == Color::rgba(255, 255, 255, 255));
    REQUIRE(rgb5a3ToColor(0x3F00) == Color::rgba(255, 0, 0, 109));
}

TEST_CASE("format properties", "[formats][texture]") {
    REQUIRE(bitsPerPixel(bitmap_format::kRgb5a3) == 16);
    REQUIRE(bitsPerPixel(bitmap_format::kIndexed4Gc) == 4);
    REQUIRE(bitsPerPixel(bitmap_format::kIndexed8Gc) == 8);
    REQUIRE(bitsPerPixel(bitmap_format::kAlpha4) == 4);
    REQUIRE(pixelDataOffset(bitmap_format::kIndexed4Gc) == 32);
    REQUIRE(pixelDataOffset(bitmap_format::kIndexed8Gc) == 512);
    REQUIRE(pixelDataOffset(bitmap_format::kIndexed8Wide) == 1024);
    REQUIRE(pixelDataOffset(bitmap_format::kAlpha4) == 0);
    REQUIRE_THROWS_AS(bitsPerPixel(200), FormatError);

    ArchiveBitmap bitmap;
    bitmap.format = bitmap_format::kIndexed8Gc;
    bitmap.width = 8;
    bitmap.height = 8;
    bitmap.mipmapCount = 1;
    REQUIRE(bitmapDataSize(bitmap) == 512 + 64 + 32);
}

TEST_CASE("direct 16-bit textures are tiled four by four", "[formats][texture]") {
    // 8x4 image: two tiles; texel (5, 1) is tile 1, row 1, column 1
    std::vector<u8> data(usize{8} * 4 * 2, 0);
    const usize texel = 16 + 1 * 4 + 1;
    data[texel * 2] = 0xFC;
    data[texel * 2 + 1] = 0x00;
    ArchiveBitmap bitmap;
    bitmap.format = bitmap_format::kRgb5a3;
    bitmap.width = 8;
    bitmap.height = 4;
    const Image image = decodeGcTexture(bitmap, data);
    REQUIRE(image.width == 8);
    REQUIRE(image.pixel(5, 1) == Color::rgba(255, 0, 0, 255));
    REQUIRE(image.pixel(4, 1) == Color::rgba(0, 0, 0, 0));
}

TEST_CASE("indexed textures resolve through their palette", "[formats][texture]") {
    std::vector<u8> data;
    for (int i = 0; i < 256; ++i) {
        putBigU16(data, i == 3 ? 0x83E0 : 0x8000); // entry 3 = green
    }
    std::vector<u8> pixels(usize{8} * 4, 0); // one 8x4 tile
    pixels[2 * 8 + 6] = 3;                   // texel (6, 2)
    data.insert(data.end(), pixels.begin(), pixels.end());

    ArchiveBitmap bitmap;
    bitmap.format = bitmap_format::kIndexed8Gc;
    bitmap.width = 8;
    bitmap.height = 4;
    const Image image = decodeGcTexture(bitmap, data);
    REQUIRE(image.pixel(6, 2) == Color::rgba(0, 255, 0, 255));
    REQUIRE(image.pixel(0, 0) == Color::rgba(0, 0, 0, 255));

    std::vector<u8> data4;
    for (int i = 0; i < 16; ++i) {
        u16 entry = 0x8000; // 1 = blue, 2 = white, rest black
        if (i == 1) {
            entry = 0x801F;
        } else if (i == 2) {
            entry = 0xFFFF;
        }
        putBigU16(data4, entry);
    }
    std::vector<u8> pixels4(8 * 8 / 2, 0); // one 8x8 tile, high nibble first
    pixels4[0] = 0x12;                     // texels (0,0) = 1, (1,0) = 2
    data4.insert(data4.end(), pixels4.begin(), pixels4.end());
    bitmap.format = bitmap_format::kIndexed4Gc;
    bitmap.height = 8;
    const Image image4 = decodeGcTexture(bitmap, data4);
    REQUIRE(image4.pixel(0, 0) == Color::rgba(0, 0, 255, 255));
    REQUIRE(image4.pixel(1, 0) == Color::rgba(255, 255, 255, 255));
    REQUIRE(image4.pixel(2, 0) == Color::rgba(0, 0, 0, 255));
}

TEST_CASE("intensity textures become white with alpha", "[formats][texture]") {
    std::vector<u8> pixels(8 * 8 / 2, 0);
    pixels[0] = 0xF8; // texel (0,0) full, (1,0) half
    ArchiveBitmap bitmap;
    bitmap.format = bitmap_format::kAlpha4;
    bitmap.width = 8;
    bitmap.height = 8;
    const Image image = decodeGcTexture(bitmap, pixels);
    REQUIRE(image.pixel(0, 0) == Color::rgba(255, 255, 255, 255));
    REQUIRE(image.pixel(1, 0) == Color::rgba(255, 255, 255, 136));
    REQUIRE(image.pixel(2, 0) == Color::rgba(255, 255, 255, 0));

    std::vector<u8> pixels8(usize{8} * 4, 0);
    pixels8[1] = 200;
    bitmap.format = bitmap_format::kIntensity8;
    bitmap.height = 4;
    const Image image8 = decodeGcTexture(bitmap, pixels8);
    REQUIRE(image8.pixel(1, 0) == Color::rgba(255, 255, 255, 200));
}

TEST_CASE("truncated pixel data is rejected", "[formats][texture]") {
    ArchiveBitmap bitmap;
    bitmap.format = bitmap_format::kRgb5a3;
    bitmap.width = 8;
    bitmap.height = 8;
    REQUIRE_THROWS_AS(decodeGcTexture(bitmap, std::vector<u8>(10, 0)), FormatError);
}

TEST_CASE("the title backdrop and glow frames decode", "[formats][texture][assets]") {
    const auto objectsPath = test::assetOrSkip("TITLE/objects.ngc");
    const auto texturesPath = test::assetOrSkip("TITLE/textures.ngc");
    const ModelArchive archive = ModelArchive::parse(readFile(objectsPath));
    const std::vector<u8> textures = readFile(texturesPath);

    const Image backdrop = decodeGcTexture(archive.bitmaps()[11], textures);
    REQUIRE(backdrop.width == 256);
    REQUIRE(backdrop.height == 256);
    u32 opaque = 0;
    for (u32 y = 0; y < backdrop.height; y += 8) {
        for (u32 x = 0; x < backdrop.width; x += 8) {
            opaque += backdrop.pixel(x, y).a == 255 ? 1 : 0;
        }
    }
    REQUIRE(opaque > 512);

    const Image glow = decodeGcTexture(archive.bitmaps()[5], textures);
    REQUIRE(glow.width == 64);
    bool translucent = false;
    for (u32 y = 0; y < glow.height && !translucent; ++y) {
        for (u32 x = 0; x < glow.width && !translucent; ++x) {
            translucent = glow.pixel(x, y).a < 255;
        }
    }
    REQUIRE(translucent);
}

} // namespace
