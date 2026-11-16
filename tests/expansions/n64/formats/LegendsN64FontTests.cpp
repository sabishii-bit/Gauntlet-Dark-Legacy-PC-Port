#include <array>
#include <cmath>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"

#include "SyntheticBytes.h"
#include "expansions/n64/formats/LegendsN64Font.h"

namespace {

using namespace gdl;
using formats::LegendsN64Font;

// A header's format and texel size bytes: the N64's own texture format and texel size codes.
constexpr u8 kRgbaFormat = 0;
constexpr u8 kIndexFormat = 2;
constexpr u8 kIntensityAlphaFormat = 3;
constexpr u8 kIntensityFormat = 4;
constexpr u8 kFourBits = 0;
constexpr u8 kEightBits = 1;
constexpr u8 kSixteenBits = 2;
constexpr u8 kThirtyTwoBits = 3;

/** The header fields these cases set. */
struct Header {
    u8 format = kIntensityFormat;
    u8 size = kFourBits;
    u16 width = 0;
    u16 height = 0;
    u16 lineHeight = 0;
};

/** One entry of a font's five-byte glyph directory. */
struct Glyph {
    u8 code = 0;
    u8 x = 0;
    u8 y = 0;
    u8 columns = 0;
    u8 rows = 0;
};

/** A native font: the 0x24-byte header, the glyph directory, then the atlas at the next sixteen
 * bytes. */
std::vector<u8> nativeFont(const Header& header, std::span<const Glyph> glyphs,
                           std::span<const u8> atlas) {
    constexpr usize kHeaderBytes = 0x24;
    constexpr usize kAtlasAlignment = 16;
    constexpr u16 kSpace = 2;
    test::SyntheticBytes bytes;
    // The slot and load band before the dimensions are not read.
    bytes.putU8(0).putU8(header.format).putU8(header.size).putZeros(5);
    bytes.putU16BE(header.width).putU16BE(header.height).putU16BE(header.lineHeight);
    bytes.putU16BE(kSpace).putU32BE(0).putU32BE(static_cast<u32>(glyphs.size()));
    bytes.putZeros(kHeaderBytes - bytes.size());
    for (const Glyph& glyph : glyphs) {
        bytes.putU8(glyph.code).putU8(glyph.x).putU8(glyph.y);
        bytes.putU8(glyph.columns).putU8(glyph.rows);
    }
    bytes.putZeros((kAtlasAlignment - bytes.size() % kAtlasAlignment) % kAtlasAlignment);
    bytes.putBytes(atlas);
    return bytes.bytes();
}

/** Texel `index` of an atlas, counted along its rows. */
Color texel(const Image& atlas, u32 index) {
    return atlas.pixel(index % atlas.width, index / atlas.width);
}

} // namespace

TEST_CASE("N64 fonts read intensity, IA4 and IA8 atlases", "[legends][n64-font]") {
    // Each atlas holds, in order, every value its texels can: sixteen on 4x4, 256 on 16x16.
    std::vector<u8> fourBit(8);
    for (usize i = 0; i < fourBit.size(); ++i) {
        const usize first = i * 2; // two texels a byte, the high nibble first
        fourBit[i] = static_cast<u8>((first << 4U) | (first + 1));
    }
    std::vector<u8> eightBit(256);
    for (usize i = 0; i < eightBit.size(); ++i) {
        eightBit[i] = static_cast<u8>(i);
    }
    const std::array small{Glyph{'A', 0, 0, 4, 4}};
    const std::array large{Glyph{'A', 0, 0, 16, 16}};

    SECTION("an I4 texel is coverage alone, drawn in the ink") {
        const auto font = LegendsN64Font::parse(
            nativeFont({.width = 4, .height = 4, .lineHeight = 4}, small, fourBit));
        REQUIRE(font.atlas.width == 4U);
        REQUIRE(font.atlas.height == 4U);
        for (u32 value = 0; value < 16; ++value) {
            CAPTURE(value);
            CHECK(texel(font.atlas, value) ==
                  Color::rgba(255, 255, 255, static_cast<u8>(value * 17)));
        }
    }

    SECTION("an IA4 texel is three bits of intensity over one of alpha") {
        const auto font = LegendsN64Font::parse(
            nativeFont({.format = kIntensityAlphaFormat, .width = 4, .height = 4, .lineHeight = 4},
                       small, fourBit));
        REQUIRE(font.atlas.width == 4U);
        REQUIRE(font.atlas.height == 4U);
        for (u32 value = 0; value < 16; ++value) {
            CAPTURE(value);
            const auto intensity = static_cast<u8>(std::lround((value >> 1U) * 255.0 / 7));
            CHECK(texel(font.atlas, value) ==
                  Color::rgba(intensity, intensity, intensity, (value & 1U) != 0 ? 255 : 0));
        }
        // Full intensity may be clear and none may cover: the alpha is never the intensity.
        CHECK(texel(font.atlas, 0b1110) == Color::rgba(255, 255, 255, 0));
        CHECK(texel(font.atlas, 0b0001) == Color::rgba(0, 0, 0, 255));
        CHECK(texel(font.atlas, 0b0101) == Color::rgba(73, 73, 73, 255));
    }

    SECTION("an IA8 texel is a byte, a nibble of intensity over one of alpha") {
        const auto bytes = nativeFont({.format = kIntensityAlphaFormat,
                                       .size = kEightBits,
                                       .width = 16,
                                       .height = 16,
                                       .lineHeight = 16},
                                      large, eightBit);
        const auto font = LegendsN64Font::parse(bytes);
        REQUIRE(font.atlas.width == 16U);
        REQUIRE(font.atlas.height == 16U);
        for (u32 value = 0; value < 256; ++value) {
            CAPTURE(value);
            const auto intensity = static_cast<u8>((value >> 4U) * 17);
            CHECK(texel(font.atlas, value) == Color::rgba(intensity, intensity, intensity,
                                                          static_cast<u8>((value & 15U) * 17)));
        }
        CHECK(texel(font.atlas, 0xF0) == Color::rgba(255, 255, 255, 0));
        CHECK(texel(font.atlas, 0x0F) == Color::rgba(0, 0, 0, 255));
        CHECK(texel(font.atlas, 0x5A) == Color::rgba(85, 85, 85, 170));
        // An atlas a byte short is refused, and so is the half that four-bit texels would fill.
        CHECK_THROWS_AS(LegendsN64Font::parse(std::span{bytes}.first(bytes.size() - 1)),
                        FormatError);
        CHECK_THROWS_AS(
            LegendsN64Font::parse(std::span{bytes}.first(bytes.size() - eightBit.size() / 2)),
            FormatError);
    }
}

TEST_CASE("a glyph may reach below its atlas into its line, never past it", "[legends][n64-font]") {
    // As numfont's digits claim an eighth row of a seven-row atlas with a line of nine: here two
    // glyphs on a 4x2 IA8 atlas, white and covered throughout, with a line of four.
    const std::vector<u8> covered(8, 0xFF);
    const Header header{.format = kIntensityAlphaFormat,
                        .size = kEightBits,
                        .width = 4,
                        .height = 2,
                        .lineHeight = 4};
    const std::array glyphs{Glyph{'0', 0, 0, 1, 3}, Glyph{'1', 1, 0, 3, 2}};
    const auto font = LegendsN64Font::parse(nativeFont(header, glyphs, covered));
    // The atlas gains clear rows down to the lowest glyph's bottom, not to the line's.
    REQUIRE(font.atlas.width == 4U);
    REQUIRE(font.atlas.height == 3U);
    REQUIRE(font.atlas.pixels.size() == usize{4 * 3 * 4});
    for (u32 x = 0; x < 4; ++x) {
        CAPTURE(x);
        CHECK(font.atlas.pixel(x, 0) == Color::white());
        CHECK(font.atlas.pixel(x, 1) == Color::white());
        CHECK(font.atlas.pixel(x, 2) == Color::transparent());
    }
    CHECK(font.font.height() == 4);
    REQUIRE(font.font.glyph('0') != nullptr);
    CHECK(font.font.glyph('0')->rows == 3);

    // A glyph may end on its line wherever it starts, but never past it, nor past the right edge.
    const auto reaching = [&](const Glyph& glyph) {
        return LegendsN64Font::parse(nativeFont(header, std::array{glyph}, covered));
    };
    CHECK(reaching({'0', 0, 0, 1, 4}).atlas.height == 4U);
    CHECK(reaching({'0', 0, 1, 1, 3}).atlas.height == 4U);
    CHECK_THROWS_AS(reaching({'0', 0, 0, 1, 5}), FormatError);
    CHECK_THROWS_AS(reaching({'0', 0, 1, 1, 4}), FormatError);
    CHECK_THROWS_AS(reaching({'0', 3, 0, 2, 1}), FormatError);

    // Within its atlas a glyph may pass its line, as most of 4x6font's, glfm3's and bigfont's
    // glyphs do: the line bounds only what reaches below the atlas.
    const std::vector<u8> tall(16, 0xFF);
    const Header tallHeader{.width = 4, .height = 8, .lineHeight = 4};
    const std::array deep{Glyph{'0', 0, 4, 1, 4}, Glyph{'1', 1, 0, 1, 8}};
    const auto within = LegendsN64Font::parse(nativeFont(tallHeader, deep, tall));
    CHECK(within.atlas.height == 8U);
    REQUIRE(within.font.glyph('1') != nullptr);
    CHECK(within.font.glyph('1')->rows == 8);
    CHECK_THROWS_AS(
        LegendsN64Font::parse(nativeFont(tallHeader, std::array{Glyph{'0', 0, 1, 1, 8}}, tall)),
        FormatError);

    // An I4 atlas gains its rows the same way: white, as the rest of it is, and clear.
    const std::vector<u8> fourBit(4, 0xFF);
    const auto coverage = LegendsN64Font::parse(
        nativeFont({.width = 4, .height = 2, .lineHeight = 4}, glyphs, fourBit));
    REQUIRE(coverage.atlas.height == 3U);
    CHECK(coverage.atlas.pixel(0, 1) == Color::white());
    CHECK(coverage.atlas.pixel(0, 2) == Color::rgba(255, 255, 255, 0));
}

TEST_CASE("other N64 font formats are refused", "[legends][n64-font]") {
    // A font that reads as IA8, with atlas bytes enough for any texel size, so that only its
    // header can refuse it.
    const std::vector<u8> atlas(usize{2} * 2 * 4, 0xFF);
    const std::array glyphs{Glyph{'A', 0, 0, 2, 2}};
    const auto font = nativeFont({.format = kIntensityAlphaFormat,
                                  .size = kEightBits,
                                  .width = 2,
                                  .height = 2,
                                  .lineHeight = 2},
                                 glyphs, atlas);
    REQUIRE_NOTHROW(LegendsN64Font::parse(font));
    struct Refused {
        std::string_view name;
        u8 format = 0;
        u8 size = 0;
    };
    const std::array refused{Refused{"CI8", kIndexFormat, kEightBits},
                             Refused{"CI4", kIndexFormat, kFourBits},
                             Refused{"I8", kIntensityFormat, kEightBits},
                             Refused{"IA16", kIntensityAlphaFormat, kSixteenBits},
                             Refused{"RGBA16", kRgbaFormat, kSixteenBits},
                             Refused{"RGBA32", kRgbaFormat, kThirtyTwoBits}};
    for (const Refused& kind : refused) {
        CAPTURE(kind.name);
        auto bytes = font;
        bytes[1] = kind.format;
        bytes[2] = kind.size;
        CHECK_THROWS_AS(LegendsN64Font::parse(bytes), FormatError);
    }
}
