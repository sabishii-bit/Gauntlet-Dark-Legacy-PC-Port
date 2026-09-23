#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "formats/TplFile.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

void putU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::uint32_t shift = 24; shift > 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

/** A library with one 4x4 image of `format` whose texel data is `texels`, at offset 32. */
std::vector<std::uint8_t> sampleTpl(std::uint32_t format, const std::vector<std::uint8_t>& texels) {
    std::vector<std::uint8_t> out;
    putU32(out, 0x0020AF30);
    putU32(out, 1);
    putU32(out, 12); // the image table follows the header
    putU32(out, 20); // whose one entry points at the image header
    putU32(out, 0);  // and has no palette
    putU16(out, 4);  // height
    putU16(out, 4);  // width
    putU32(out, format);
    putU32(out, 32);
    out.resize(32, 0);
    out.insert(out.end(), texels.begin(), texels.end());
    return out;
}

TEST_CASE("RGB5A3 tiles decode with their alpha", "[formats][tpl]") {
    std::vector<std::uint8_t> texels;
    for (std::uint32_t i = 0; i < 16; ++i) {
        std::uint16_t texel = 0x0000;
        if (i == 0) {
            texel = 0x801F; // opaque blue
        } else if (i == 5) {
            texel = 0x3F00; // red at alpha 3 of 7
        }
        putU16(texels, texel);
    }
    const std::vector<TplImage> images = parseTplFile(sampleTpl(kTplRgb5a3, texels));
    REQUIRE(images.size() == 1);
    REQUIRE(images[0].format == kTplRgb5a3);
    REQUIRE(images[0].image.width == 4);
    REQUIRE(images[0].image.height == 4);
    REQUIRE(images[0].image.pixel(0, 0) == Color::rgba(0, 0, 255));
    REQUIRE(images[0].image.pixel(1, 1) == Color::rgba(255, 0, 0, 109)); // texel 5 of the tile
    REQUIRE(images[0].image.pixel(3, 3) == Color::rgba(0, 0, 0, 0));
}

TEST_CASE("RGB565 and RGBA8 tiles decode", "[formats][tpl]") {
    std::vector<std::uint8_t> texels;
    putU16(texels, 0xFFFF);
    putU16(texels, 0xF800);
    texels.resize(32, 0);
    const std::vector<TplImage> rgb565 = parseTplFile(sampleTpl(kTplRgb565, texels));
    REQUIRE(rgb565[0].image.pixel(0, 0) == Color::white());
    REQUIRE(rgb565[0].image.pixel(1, 0) == Color::rgba(255, 0, 0));
    REQUIRE(rgb565[0].image.pixel(2, 0) == Color::rgba(0, 0, 0));

    // One 64-byte tile: an alpha/red plane, then a green/blue plane; texel 2 is (2, 0).
    std::vector<std::uint8_t> planes(64, 0);
    planes[4] = 0x80;
    planes[5] = 0x10;
    planes[32 + 4] = 0x20;
    planes[32 + 5] = 0x30;
    const std::vector<TplImage> rgba8 = parseTplFile(sampleTpl(kTplRgba8, planes));
    REQUIRE(rgba8[0].image.pixel(2, 0) == Color::rgba(0x10, 0x20, 0x30, 0x80));
    REQUIRE(rgba8[0].image.pixel(3, 3) == Color::rgba(0, 0, 0, 0));
}

TEST_CASE("malformed libraries are refused", "[formats][tpl]") {
    const std::vector<std::uint8_t> texels(32, 0);
    std::vector<std::uint8_t> bad = sampleTpl(kTplRgb5a3, texels);
    bad[0] = 0xFF;
    REQUIRE_THROWS_AS(parseTplFile(bad), FormatError);

    std::vector<std::uint8_t> truncated = sampleTpl(kTplRgb5a3, texels);
    truncated.resize(40);
    REQUIRE_THROWS_AS(parseTplFile(truncated), FormatError);

    REQUIRE_THROWS_AS(parseTplFile(sampleTpl(9, texels)), FormatError);
    REQUIRE_THROWS_AS(parseTplFile(std::vector<std::uint8_t>{1, 2, 3}), FormatError);
}

TEST_CASE("the memory-card icon has eight translucent frames", "[formats][tpl][assets]") {
    // The card art sits beside the game folder on the disc, outside the asset locator's root.
    const std::filesystem::path file =
        std::filesystem::path(GDL_TEST_ASSET_DIR).parent_path() / "carddemo" / "icon.tpl";
    if (!std::filesystem::exists(file)) {
        SKIP("the memory-card art is not installed");
    }
    const std::vector<TplImage> frames = parseTplFile(readFile(file));
    REQUIRE(frames.size() == 8);
    std::uint8_t minAlpha = 255;
    std::uint8_t maxAlpha = 0;
    for (const TplImage& frame : frames) {
        REQUIRE(frame.format == kTplRgb5a3);
        REQUIRE(frame.image.width == 32);
        REQUIRE(frame.image.height == 32);
        for (std::uint32_t y = 0; y < 32; ++y) {
            for (std::uint32_t x = 0; x < 32; ++x) {
                const std::uint8_t alpha = frame.image.pixel(x, y).a;
                minAlpha = std::min(minAlpha, alpha);
                maxAlpha = std::max(maxAlpha, alpha);
            }
        }
    }
    // The flame fades out at its edges and never quite reaches full opacity.
    REQUIRE(minAlpha == 0);
    REQUIRE(maxAlpha > 128);
}

} // namespace
