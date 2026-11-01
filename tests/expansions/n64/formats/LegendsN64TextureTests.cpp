#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
#include "expansions/n64/formats/LegendsN64Texture.h"

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

struct TextureFixture {
    std::vector<u8> bytes;
    static constexpr usize kHeader = 8;
    static constexpr usize kName = 32;
    static constexpr usize kLoader = 40;
    static constexpr usize kPixels = 80;
    usize directory = 0;

    explicit TextureFixture(u8 format, u8 size, usize pixelBytes = 520)
        : bytes(kPixels + pixelBytes + usize{4} * 12), directory(kPixels + pixelBytes) {
        put32(bytes, 0, static_cast<u32>(directory));
        put32(bytes, 4, 4);
        put32(bytes, kHeader, 0);
        put32(bytes, kHeader + 4, kLoader);
        put16(bytes, kHeader + 12, 2);
        put16(bytes, kHeader + 14, 1);
        put16(bytes, kHeader + 16, 1);
        bytes[kHeader + 18] = format;
        bytes[kHeader + 19] = size;
        put16(bytes, kHeader + 22, 0xFFFF);
        bytes[kName] = 'A';
        put32(bytes, kLoader, 0xFD100000);
        put32(bytes, kLoader + 8, 0xF5000000);
        put32(bytes, kLoader + 16, 0xDF000000);
        const std::array<std::string_view, 4> tags{"TXHD", "NAME", "TXLD", "TEXR"};
        const std::array<u32, 4> offsets{kHeader, kName, kLoader, kPixels};
        const std::array<u32, 4> counts{1, 8, 40, static_cast<u32>(pixelBytes)};
        for (usize i = 0; i < tags.size(); ++i) {
            const auto at = directory + i * 12;
            std::copy(tags[i].begin(), tags[i].end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(at));
            put32(bytes, at + 4, offsets[i]);
            put32(bytes, at + 8, counts[i]);
        }
    }

    LegendsN64Texture decode() const { return decodeLegendsN64Textures(bytes).at(0); }
};

struct ScrollFixture {
    TextureFixture texture{0, 2};
    usize scroll = texture.directory;
    usize directory = scroll + 20;

    ScrollFixture() {
        const auto oldDirectory = texture.directory;
        const std::vector<u8> entries(
            texture.bytes.begin() + static_cast<std::ptrdiff_t>(oldDirectory), texture.bytes.end());
        texture.bytes.resize(directory + usize{5} * 12);
        std::fill(texture.bytes.begin() + static_cast<std::ptrdiff_t>(scroll), texture.bytes.end(),
                  0);
        std::copy(entries.begin(), entries.end(),
                  texture.bytes.begin() + static_cast<std::ptrdiff_t>(directory));
        put32(texture.bytes, 0, static_cast<u32>(directory));
        put32(texture.bytes, 4, 5);
        const auto entry = directory + usize{4} * 12;
        const std::string_view tag = "TSCR";
        std::copy(tag.begin(), tag.end(),
                  texture.bytes.begin() + static_cast<std::ptrdiff_t>(entry));
        put32(texture.bytes, entry + 4, static_cast<u32>(scroll));
        put32(texture.bytes, entry + 8, 1);
        put16(texture.bytes, TextureFixture::kHeader + 12, 64);
        put16(texture.bytes, TextureFixture::kHeader + 14, 32);
        put32(texture.bytes, TextureFixture::kLoader + 16, 0xF2000000);
        put32(texture.bytes, TextureFixture::kLoader + 20, 0x000FC07C);
        put32(texture.bytes, TextureFixture::kLoader + 24, 0xDF000000);
        put32(texture.bytes, scroll, TextureFixture::kLoader + 16);
        rate(0, 0.3f);
        rate(1, -1.0f);
    }

    void rate(usize axis, f32 value) {
        put32(texture.bytes, scroll + 4 + axis * 4, std::bit_cast<u32>(value));
    }
    LegendsN64TextureScroll decode() const {
        return decodeLegendsN64TextureScrolls(texture.bytes).at(0);
    }
};

TEST_CASE("Legends N64 TSCR records preserve native rates phases and target tile",
          "[formats][legends][texture-scroll]") {
    ScrollFixture fixture;
    put16(fixture.texture.bytes, fixture.scroll + 14, 0xFFF9);
    put16(fixture.texture.bytes, fixture.scroll + 16, 9);
    put16(fixture.texture.bytes, fixture.scroll + 18, 0x1234);
    const auto before = fixture.texture.bytes;
    const auto scroll = fixture.decode();
    CHECK(scroll.texture == 0);
    CHECK(scroll.commandOffset == TextureFixture::kLoader + 16);
    CHECK(scroll.width == 64);
    CHECK(scroll.height == 32);
    CHECK(scroll.texelsPerTick.x == Catch::Approx(0.3f));
    CHECK(scroll.texelsPerTick.y == -1.0f);
    CHECK(scroll.initialPhase[0] == -7);
    CHECK(scroll.initialPhase[1] == 9);
    CHECK(scroll.reserved == 0x1234);
    const LegendsN64TextureScrollState state(scroll);
    CHECK(state.offset(scroll).x == Catch::Approx(0.25f / 64.0f));
    CHECK(state.offset(scroll).y == Catch::Approx(-0.25f / 32.0f));
    CHECK(fixture.texture.bytes == before);
    TextureFixture noScroll(0, 2);
    CHECK(decodeLegendsN64TextureScrolls(noScroll.bytes).empty());
}

TEST_CASE("Legends N64 scrolling retains signed native truncation across presentation rates",
          "[formats][legends][texture-scroll]") {
    const auto scroll = ScrollFixture{}.decode();
    LegendsN64TextureScrollState stepped(scroll);
    stepped.advance(scroll, 1.0f / 30.0f);
    CHECK(stepped.phase()[0] == 9);
    CHECK(stepped.phase()[1] == -32);
    CHECK(stepped.offset(scroll).x == Catch::Approx(-0.25f / 64.0f));
    CHECK(stepped.offset(scroll).y == Catch::Approx(1.0f / 32.0f));
    for (u32 i = 1; i < 8; ++i) {
        stepped.advance(scroll, 1.0f / 30.0f);
    }
    CHECK(stepped.phase()[0] == -1976); // 72 exceeds the native upper comparison, 64.
    stepped.advance(scroll, 1.0f / 30.0f);
    CHECK(stepped.phase()[0] == -1966); // trunc(-1976 + 9.6), not -1976 + trunc(9.6).
    LegendsN64TextureScrollState reference(scroll);
    reference.advance(scroll, 10.0f);
    for (const u32 frequency : {30U, 60U, 144U}) {
        CAPTURE(frequency);
        LegendsN64TextureScrollState state(scroll);
        for (u32 frame = 0; frame < frequency * 10; ++frame) {
            state.advance(scroll, 1.0f / static_cast<f32>(frequency));
        }
        CHECK(state.phase() == reference.phase());
        CHECK(state.offset(scroll) == reference.offset(scroll));
        const auto phase = state.phase();
        for (const auto interval : {-1.0f, 0.0f, 61.0f, std::numeric_limits<f32>::infinity(),
                                    std::numeric_limits<f32>::quiet_NaN()}) {
            state.advance(scroll, interval);
        }
        CHECK(state.phase() == phase);
    }
    auto waterfall = scroll;
    waterfall.texelsPerTick = Vec2{0.0f, -2.0f};
    LegendsN64TextureScrollState wrapped(waterfall);
    wrapped.advance(waterfall, 16.0f / 30.0f);
    CHECK(wrapped.phase()[1] == 0);
    CHECK(wrapped.offset(waterfall).y == 0.0f);
}

TEST_CASE("Legends N64 scrolling rejects malformed native bindings",
          "[formats][legends][texture-scroll]") {
    ScrollFixture fixture;
    SECTION("missing target slot") {
        put16(fixture.texture.bytes, fixture.scroll + 12, 1);
    }
    SECTION("truncated scroll") {
        put32(fixture.texture.bytes, fixture.directory + usize{4} * 12 + 8, 2);
    }
    SECTION("nonfinite speed") {
        fixture.rate(1, std::numeric_limits<f32>::infinity());
    }
    SECTION("invalid signed phase range") {
        put16(fixture.texture.bytes, TextureFixture::kHeader + 12, 2048);
    }
    SECTION("zero texture extent") {
        put16(fixture.texture.bytes, TextureFixture::kHeader + 12, 0);
    }
    SECTION("speed exceeds a tile") {
        fixture.rate(0, 65.0f);
    }
    SECTION("outside loader") {
        put32(fixture.texture.bytes, fixture.scroll, TextureFixture::kPixels);
    }
    SECTION("unaligned command") {
        put32(fixture.texture.bytes, fixture.scroll, TextureFixture::kLoader + 17);
    }
    SECTION("wrong command") {
        put32(fixture.texture.bytes, TextureFixture::kLoader + 16, 0xDF000000);
    }
    SECTION("load tile instead of render tile") {
        put32(fixture.texture.bytes, TextureFixture::kLoader + 20, 0x070FC07C);
    }
    CHECK_THROWS_AS(fixture.decode(), FormatError);
}

TEST_CASE("native N64 scrolling covers Tower lava snow gas and level waterfalls",
          "[formats][legends][native-assets][texture-scroll]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root)) {
        SKIP("native N64 extraction is not installed");
    }
    usize total = 0;
    usize levelRecords = 0;
    for (const auto& file : std::filesystem::recursive_directory_iterator(root)) {
        if (!file.is_regular_file() || file.path().filename() != "objects.wad") {
            continue;
        }
        CAPTURE(file.path().string());
        const auto bytes = readFile(file.path());
        const auto scrolls = decodeLegendsN64TextureScrolls(bytes);
        total += scrolls.size();
        if (file.path().parent_path().parent_path().filename() == "levels") {
            levelRecords += scrolls.size();
        }
        for (const auto& scroll : scrolls) {
            LegendsN64TextureScrollState state(scroll);
            const auto initial = state.offset(scroll);
            state.advance(scroll, 1.0f);
            CHECK(std::isfinite(state.offset(scroll).x));
            CHECK(std::isfinite(state.offset(scroll).y));
            CHECK(state.offset(scroll) != initial);
        }
        if (file.path().parent_path().filename() == "tower2") {
            REQUIRE(scrolls.size() == 3);
            CHECK(scrolls[0].texture == 32);
            CHECK(scrolls[0].texelsPerTick.y == -0.5f);
            CHECK(scrolls[1].texture == 114);
            CHECK(scrolls[1].texelsPerTick.y == -1.0f);
            CHECK(scrolls[2].texture == 117);
            CHECK(scrolls[2].texelsPerTick.y == 0.5f);
        }
        if (file.path().parent_path().filename() == "ice3") {
            REQUIRE(scrolls.size() == 2);
            CHECK(scrolls[0].texture == 63);
            CHECK(scrolls[0].texelsPerTick.y == -2.0f);
        }
    }
    CHECK(total == 46);
    CHECK(levelRecords == 42);
}

TEST_CASE("Legends N64 native direct-color textures preserve color and alpha",
          "[formats][legends][texture]") {
    SECTION("RGBA5551") {
        TextureFixture fixture(0, 2);
        put16(fixture.bytes, TextureFixture::kPixels, 0xF801);
        put16(fixture.bytes, TextureFixture::kPixels + 2, 0x07C0);
        const auto texture = fixture.decode();
        CHECK(texture.name == "A");
        CHECK(texture.image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(0, 255, 0, 0));
        CHECK(texture.hasTransparency);
    }
    SECTION("RGBA8888") {
        TextureFixture fixture(0, 3);
        put32(fixture.bytes, TextureFixture::kPixels, 0x12345678);
        put32(fixture.bytes, TextureFixture::kPixels + 4, 0xABCDEF00);
        const auto before = fixture.bytes;
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(0x12, 0x34, 0x56, 0x78));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(0xAB, 0xCD, 0xEF, 0));
        CHECK(fixture.bytes == before);
    }
    SECTION("IA8") {
        TextureFixture fixture(3, 1);
        fixture.bytes[TextureFixture::kPixels] = 0x8F;
        fixture.bytes[TextureFixture::kPixels + 1] = 0xA4;
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(136, 136, 136, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(170, 170, 170, 68));
    }
    SECTION("IA4") {
        TextureFixture fixture(3, 0);
        fixture.bytes[TextureFixture::kPixels] = 0x5C;
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(73, 73, 73, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(219, 219, 219, 0));
    }
    SECTION("IA16") {
        TextureFixture fixture(3, 2);
        put32(fixture.bytes, TextureFixture::kPixels, 0x12345678);
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(0x12, 0x12, 0x12, 0x34));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(0x56, 0x56, 0x56, 0x78));
    }
    SECTION("I8") {
        TextureFixture fixture(4, 1);
        fixture.bytes[TextureFixture::kPixels] = 0x21;
        fixture.bytes[TextureFixture::kPixels + 1] = 0xF8;
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(0x21, 0x21, 0x21, 0x21));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(0xF8, 0xF8, 0xF8, 0xF8));
    }
    SECTION("I4") {
        TextureFixture fixture(4, 0);
        fixture.bytes[TextureFixture::kPixels] = 0xF7;
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(255, 255, 255, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(119, 119, 119, 119));
    }
}

TEST_CASE("Legends N64 extended intensity formats retain native material-independent pixels",
          "[formats][legends][texture]") {
    SECTION("native format six is I4") {
        TextureFixture fixture(6, 0);
        fixture.bytes[TextureFixture::kPixels] = 0xA5;
        const auto texture = fixture.decode();
        CHECK(texture.format == 6);
        CHECK(texture.image.pixel(0, 0) == Color::rgba(170, 170, 170, 170));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(85, 85, 85, 85));
    }
    SECTION("native format seven is IA8") {
        TextureFixture fixture(7, 1);
        fixture.bytes[TextureFixture::kPixels] = 0x8F;
        fixture.bytes[TextureFixture::kPixels + 1] = 0xA4;
        const auto texture = fixture.decode();
        CHECK(texture.format == 7);
        CHECK(texture.image.pixel(0, 0) == Color::rgba(136, 136, 136, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(170, 170, 170, 68));
    }
}

TEST_CASE("N64 direct textures honor padded rows and RGBA32 split TMEM banks",
          "[formats][legends][texture][native-row-pitch]") {
    SECTION("RGBA32 line words cover two banks") {
        TextureFixture fixture(0, 3, 32);
        put16(fixture.bytes, TextureFixture::kHeader + 14, 2);
        put16(fixture.bytes, TextureFixture::kHeader + 16, 4);
        put32(fixture.bytes, TextureFixture::kLoader + 8, 0xF5000200);
        put32(fixture.bytes, TextureFixture::kPixels, 0x12345678);
        put32(fixture.bytes, TextureFixture::kPixels + 4, 0xABCDEF00);
        put32(fixture.bytes, TextureFixture::kPixels + 8, 0xFFFFFFFF); // Padding.
        put32(fixture.bytes, TextureFixture::kPixels + 16, 0x87654321);
        put32(fixture.bytes, TextureFixture::kPixels + 20, 0xFEDCBAFF);
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(0x12, 0x34, 0x56, 0x78));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(0xAB, 0xCD, 0xEF, 0));
        CHECK(texture.image.pixel(0, 1) == Color::rgba(0x87, 0x65, 0x43, 0x21));
        CHECK(texture.image.pixel(1, 1) == Color::rgba(0xFE, 0xDC, 0xBA, 0xFF));
    }
    SECTION("IA8 scanlines skip padding without changing alpha") {
        TextureFixture fixture(3, 1, 16);
        put16(fixture.bytes, TextureFixture::kHeader + 14, 2);
        put16(fixture.bytes, TextureFixture::kHeader + 16, 2);
        put32(fixture.bytes, TextureFixture::kLoader + 8, 0xF5000200);
        put16(fixture.bytes, TextureFixture::kPixels, 0x8FA4);
        put16(fixture.bytes, TextureFixture::kPixels + 2, 0xFFFF); // Padding.
        put16(fixture.bytes, TextureFixture::kPixels + 8, 0x3B70);
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(136, 136, 136, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(170, 170, 170, 68));
        CHECK(texture.image.pixel(0, 1) == Color::rgba(51, 51, 51, 187));
        CHECK(texture.image.pixel(1, 1) == Color::rgba(119, 119, 119, 0));
    }
}

TEST_CASE("Legends N64 paired CI8 palette alpha retains all four authored levels",
          "[formats][legends][texture]") {
    TextureFixture fixture(5, 1);
    put16(fixture.bytes, TextureFixture::kHeader + 12, 4);
    // High nibble supplies A1; the full index supplies RGB and A0.
    constexpr std::array<u8, 4> kIndices{0x10, 0x20, 0x30, 0x40};
    std::copy(kIndices.begin(), kIndices.end(), fixture.bytes.begin() + TextureFixture::kPixels);
    const auto palette = TextureFixture::kPixels + 8;
    put16(fixture.bytes, palette + usize{1} * 2, 0);
    put16(fixture.bytes, palette + usize{2} * 2, 1);
    put16(fixture.bytes, palette + usize{3} * 2, 0);
    put16(fixture.bytes, palette + usize{4} * 2, 1);
    put16(fixture.bytes, palette + usize{0x10} * 2, 0xF800);
    put16(fixture.bytes, palette + usize{0x20} * 2, 0xF800);
    put16(fixture.bytes, palette + usize{0x30} * 2, 0xF801);
    put16(fixture.bytes, palette + usize{0x40} * 2, 0xF801);
    const auto texture = fixture.decode();
    CHECK(texture.format == 5);
    for (u32 i = 0; i < 4; ++i) {
        CHECK(texture.image.pixel(i, 0) == Color::rgba(255, 0, 0, static_cast<u8>(i * 85)));
    }
}

TEST_CASE("Legends N64 CI textures resolve native RGBA16 palettes", "[formats][legends][texture]") {
    SECTION("CI4 palette follows allocated pixel storage") {
        TextureFixture fixture(2, 0);
        fixture.bytes[TextureFixture::kPixels] = 0x12;
        put16(fixture.bytes, TextureFixture::kPixels + 8 + 2, 0xF801);
        put16(fixture.bytes, TextureFixture::kPixels + 8 + 4, 0x003F);
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(0, 0, 255, 255));
        CHECK_FALSE(texture.hasTransparency);
    }
    SECTION("CI8 palette uses all 256 indices") {
        TextureFixture fixture(2, 1);
        fixture.bytes[TextureFixture::kPixels] = 255;
        fixture.bytes[TextureFixture::kPixels + 1] = 128;
        put16(fixture.bytes, TextureFixture::kPixels + 8 + usize{255} * 2, 0xFFFF);
        put16(fixture.bytes, TextureFixture::kPixels + 8 + usize{128} * 2, 0x07C1);
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(255, 255, 255, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(0, 255, 0, 255));
    }
    SECTION("a texture strip reuses the parent palette selected by its loader") {
        TextureFixture fixture(2, 0);
        fixture.bytes[TextureFixture::kPixels] = 0x11;
        put32(fixture.bytes, TextureFixture::kLoader + 16, 0xFD100000);
        put32(fixture.bytes, TextureFixture::kLoader + 20, 64);
        put32(fixture.bytes, TextureFixture::kLoader + 24, 0xDF000000);
        put16(fixture.bytes, TextureFixture::kPixels + 64 + 2, 0xFFC1);
        const auto texture = fixture.decode();
        CHECK(texture.image.pixel(0, 0) == Color::rgba(255, 255, 0, 255));
        CHECK(texture.image.pixel(1, 0) == Color::rgba(255, 255, 0, 255));
    }
}

TEST_CASE("Legends N64 texture loaders supply clamp modes without rewriting native flags",
          "[formats][legends][texture]") {
    TextureFixture fixture(0, 3);
    SECTION("ordinary display list") {
        put32(fixture.bytes, TextureFixture::kLoader + 12, 0x200);
        const auto texture = fixture.decode();
        CHECK(texture.clampU);
        CHECK_FALSE(texture.clampV);
    }
    SECTION("compact template display list") {
        put32(fixture.bytes, TextureFixture::kHeader + 8, 0x18000005);
        put32(fixture.bytes, TextureFixture::kLoader, 0xDB060030);
        put32(fixture.bytes, TextureFixture::kLoader + 8, 0xE0020135);
        const auto texture = fixture.decode();
        CHECK(texture.clampU);
        CHECK(texture.clampV);
        CHECK(texture.flags == 0x18000005);
    }
}

TEST_CASE("Native texture rows follow the render tile pitch without decoding padding",
          "[formats][legends][texture][native-plague]") {
    TextureFixture fixture(2, 0);
    put16(fixture.bytes, TextureFixture::kHeader + 12, 10);
    put16(fixture.bytes, TextureFixture::kHeader + 14, 2);
    put16(fixture.bytes, TextureFixture::kHeader + 16, 2); // two eight-byte rows
    put32(fixture.bytes, TextureFixture::kLoader + 8, 0xF5400200);
    std::fill_n(fixture.bytes.begin() + TextureFixture::kPixels, 16, 0xee);
    std::fill_n(fixture.bytes.begin() + TextureFixture::kPixels, 5, 0x11);
    std::fill_n(fixture.bytes.begin() + TextureFixture::kPixels + 8, 5, 0x22);
    const auto palette = TextureFixture::kPixels + 16;
    put16(fixture.bytes, palette + 2, 0xf801);
    put16(fixture.bytes, palette + 4, 0x003f);
    SECTION("padded rows use their own start, with palette after allocated storage") {
        const auto decoded = fixture.decode();
        for (u32 x = 0; x < 10; ++x) {
            CHECK(decoded.image.pixel(x, 0) == Color::rgba(255, 0, 0, 255));
            CHECK(decoded.image.pixel(x, 1) == Color::rgba(0, 0, 255, 255));
        }
    }
    SECTION("a row pitch escaping the source allocation is rejected") {
        put32(fixture.bytes, TextureFixture::kLoader + 8, 0xF5400400);
        CHECK_THROWS_AS(fixture.decode(), FormatError);
    }
}

TEST_CASE("Legends N64 borrowed and dynamic textures retain their native slot",
          "[formats][legends][texture]") {
    TextureFixture fixture(0, 3);
    SECTION("borrowed texture") {
        put32(fixture.bytes, TextureFixture::kHeader, 0xFFFFFFFE);
    }
    SECTION("missing texdyn data") {
        put32(fixture.bytes, TextureFixture::kHeader + 8, 0x80);
    }
    const auto texture = fixture.decode();
    CHECK(texture.external);
    CHECK(texture.image.pixels.empty());
    CHECK(texture.name == "A");
    CHECK(texture.width == 2);
}

TEST_CASE("Legends N64 dynamic texture pixels are resolved from texdyn only when supplied",
          "[formats][legends][texture]") {
    TextureFixture fixture(0, 3);
    put32(fixture.bytes, TextureFixture::kHeader + 8, 0x80);
    const std::array<u8, 8> dynamic{1, 2, 3, 255, 4, 5, 6, 255};
    const auto texture = decodeLegendsN64Textures(fixture.bytes, dynamic).at(0);
    CHECK_FALSE(texture.external);
    CHECK(texture.image.pixel(1, 0) == Color::rgba(4, 5, 6, 255));
}

TEST_CASE("Legends N64 streamed palettes use their frame-relative segment address",
          "[formats][legends][texture]") {
    TextureFixture fixture(2, 1);
    put32(fixture.bytes, TextureFixture::kHeader, 16);
    put32(fixture.bytes, TextureFixture::kHeader + 8, 0x80);
    put32(fixture.bytes, TextureFixture::kLoader + 16, 0xFD100000);
    put32(fixture.bytes, TextureFixture::kLoader + 20, 0x0C000008);
    put32(fixture.bytes, TextureFixture::kLoader + 24, 0xDF000000);
    std::vector<u8> dynamic(16 + 8 + 512);
    dynamic[16] = 1;
    dynamic[17] = 2;
    put16(dynamic, 24 + 2, 0xF801);
    put16(dynamic, 24 + 4, 0x07C1);
    const auto texture = decodeLegendsN64Textures(fixture.bytes, dynamic).at(0);
    CHECK_FALSE(texture.external);
    CHECK(texture.paletteAddress == 0x0C000008);
    CHECK(texture.image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
    CHECK(texture.image.pixel(1, 0) == Color::rgba(0, 255, 0, 255));
}

TEST_CASE("Legends N64 drawable palettes remain external instead of choosing a player colour",
          "[formats][legends][texture]") {
    TextureFixture fixture(2, 1);
    put32(fixture.bytes, TextureFixture::kLoader + 16, 0xFD100000);
    put32(fixture.bytes, TextureFixture::kLoader + 20, 0x0A000000);
    put32(fixture.bytes, TextureFixture::kLoader + 24, 0xDF000000);
    const auto texture = fixture.decode();
    CHECK(texture.external);
    CHECK(texture.externalPalette);
    CHECK(texture.paletteAddress == 0x0A000000);
    CHECK(texture.image.pixels.empty());
}

TEST_CASE("Legends N64 texture decoder rejects malformed records", "[formats][legends][texture]") {
    TextureFixture fixture(0, 3);
    SECTION("directory outside file") {
        put32(fixture.bytes, 0, 0xFFFFFFF0);
    }
    SECTION("record count crosses another section") {
        put32(fixture.bytes, fixture.directory + 8, 2);
    }
    SECTION("pixel section overlaps directory") {
        put32(fixture.bytes, fixture.directory + usize{3} * 12 + 8, 10000);
    }
    SECTION("missing name terminator") {
        std::fill(fixture.bytes.begin() + TextureFixture::kName,
                  fixture.bytes.begin() + TextureFixture::kLoader, u8{'A'});
    }
    SECTION("name offset outside NAME") {
        put16(fixture.bytes, TextureFixture::kHeader + 20, 200);
    }
    SECTION("unsupported format") {
        fixture.bytes[TextureFixture::kHeader + 18] = 1;
    }
    SECTION("unsupported extended size") {
        fixture.bytes[TextureFixture::kHeader + 18] = 5;
    }
    SECTION("pixels exceed their own allocation") {
        put16(fixture.bytes, TextureFixture::kHeader + 16, 0);
    }
    SECTION("zero dimension") {
        put16(fixture.bytes, TextureFixture::kHeader + 12, 0);
    }
    SECTION("oversized dimension") {
        put16(fixture.bytes, TextureFixture::kHeader + 12, 65535);
    }
    SECTION("pixel offset outside TEXR") {
        put32(fixture.bytes, TextureFixture::kHeader, 10000);
    }
    SECTION("loader address outside TXLD") {
        put32(fixture.bytes, TextureFixture::kHeader + 4, TextureFixture::kPixels);
    }
    SECTION("unterminated loader") {
        std::fill(fixture.bytes.begin() + TextureFixture::kLoader,
                  fixture.bytes.begin() + TextureFixture::kPixels, 0);
    }
    SECTION("truncated palette") {
        fixture.bytes[TextureFixture::kHeader + 18] = 2;
        fixture.bytes[TextureFixture::kHeader + 19] = 1;
        put32(fixture.bytes, TextureFixture::kHeader, 8);
    }
    SECTION("unsupported palette segment") {
        fixture.bytes[TextureFixture::kHeader + 18] = 2;
        fixture.bytes[TextureFixture::kHeader + 19] = 1;
        put32(fixture.bytes, TextureFixture::kLoader + 16, 0xFD100000);
        put32(fixture.bytes, TextureFixture::kLoader + 20, 0x0B000000);
        put32(fixture.bytes, TextureFixture::kLoader + 24, 0xDF000000);
    }
    REQUIRE_THROWS_AS(fixture.decode(), FormatError);
}

TEST_CASE("all extracted N64 level texture archives decode without changing native bytes",
          "[formats][legends][native-assets][texture]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted/levels";
    if (!std::filesystem::is_directory(root)) {
        SKIP("native N64 extraction is not installed");
    }
    usize archives = 0;
    usize maps = 0;
    usize decoded = 0;
    usize borrowed = 0;
    for (const auto& directory : std::filesystem::directory_iterator(root)) {
        const auto file = directory.path() / "objects.wad";
        if (!std::filesystem::is_regular_file(file)) {
            continue;
        }
        INFO(file.string());
        const auto bytes = readFile(file);
        const auto textures = decodeLegendsN64Textures(bytes);
        REQUIRE_FALSE(textures.empty());
        for (const auto& texture : textures) {
            if (texture.external) {
                ++borrowed;
            } else {
                CHECK(texture.image.pixels.size() == usize{texture.width} * texture.height * 4);
                ++decoded;
            }
        }
        ++archives;
        if (std::filesystem::is_regular_file(directory.path() / "worlds.wad")) {
            ++maps;
        } else {
            CHECK(directory.path().filename() == "effects");
        }
    }
    CHECK(archives == 35);
    CHECK(maps == 34);
    CHECK(decoded > 1000);
    CHECK(borrowed > 0);
}

TEST_CASE("all extracted N64 fixture texture libraries decode their native formats",
          "[formats][legends][native-assets][texture-corpus]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root)) {
        SKIP("native N64 extraction is not installed");
    }
    usize archives = 0;
    std::array<usize, 8> formats{};
    for (const auto& file : std::filesystem::recursive_directory_iterator(root)) {
        if (!file.is_regular_file() || file.path().filename() != "objects.wad") {
            continue;
        }
        CAPTURE(file.path().string());
        const auto bytes = readFile(file.path());
        std::vector<u8> dynamic;
        const auto dynamicPath = file.path().parent_path() / "texdyn.rom";
        if (std::filesystem::is_regular_file(dynamicPath)) {
            dynamic = readFile(dynamicPath);
        }
        std::vector<LegendsN64Texture> textures;
        REQUIRE_NOTHROW(textures = decodeLegendsN64Textures(bytes, dynamic));
        for (const auto& texture : textures) {
            REQUIRE(texture.format < formats.size());
            ++formats[texture.format];
            if (!texture.external) {
                CHECK(texture.image.pixels.size() == usize{texture.width} * texture.height * 4);
            }
        }
        ++archives;
    }
    CHECK(archives == 215);
    CHECK(formats[5] == 416);
    CHECK(formats[6] == 30);
    CHECK(formats[7] == 864);
}

TEST_CASE("N64 color ramp conversion isolates shade-free parts and preserves native pixels",
          "[formats][legends][color-ramp]") {
    LegendsN64Texture source;
    source.name = "GRAY";
    source.width = 3;
    source.height = 1;
    source.image = {3, 1, {0, 0, 0, 7, 128, 128, 128, 99, 255, 255, 255, 255}};
    LegendsN64Model model;
    model.name = "MODEL";
    model.mesh.prelit = true;
    model.mesh.vertices.resize(3);
    model.mesh.vertices[0].color = Color::rgba(0, 120, 203, 128);
    model.mesh.parts = {{0, 0, {0, 1, 2}}, {0, 0, {0, 1, 2}}};
    model.vertexSources = {3, 4, 5};
    model.colorRamps = {{0, Color::rgba(128, 16, 0, 255), Color::rgba(255, 255, 0, 255)}};
    std::vector<LegendsN64Model> models{model};
    std::vector<LegendsN64Texture> textures{source};
    AnimationFile animation;
    resolveLegendsN64ColorRamps(models, textures, animation);
    REQUIRE(textures.size() == 2);
    CHECK(textures[0].image.pixels == source.image.pixels);
    CHECK(textures[1].image.pixel(0, 0) == Color::rgba(128, 16, 0, 7));
    CHECK(textures[1].image.pixel(1, 0) == Color::rgba(192, 136, 0, 99));
    CHECK(textures[1].image.pixel(2, 0) == Color::rgba(255, 255, 0, 255));
    REQUIRE(models[0].mesh.vertices.size() == 6);
    CHECK(models[0].mesh.parts[0].texture == 1);
    CHECK(models[0].mesh.parts[1].texture == 0);
    CHECK(models[0].mesh.parts[0].indices == std::vector<u32>{3, 4, 5});
    CHECK(models[0].mesh.parts[1].indices == std::vector<u32>{0, 1, 2});
    CHECK(models[0].mesh.vertices[0].color == model.mesh.vertices[0].color);
    CHECK(models[0].mesh.vertices[3].color == Color::white());
    CHECK(models[0].vertexSources == std::vector<u32>{3, 4, 5, 3, 4, 5});
    resolveLegendsN64ColorRamps(models, textures, animation);
    CHECK(textures.size() == 2);
    CHECK(models[0].mesh.vertices.size() == 6);

    SECTION("unresolved external pixels retain original material and metadata") {
        models = {model};
        textures = {source};
        textures[0].external = true;
        std::vector<std::string> warnings;
        resolveLegendsN64ColorRamps(models, textures, animation, &warnings);
        CHECK(warnings.size() == 1);
        CHECK(textures.size() == 1);
        CHECK(models[0].mesh.vertices == model.mesh.vertices);
        CHECK(models[0].mesh.parts[0].texture == 0);
        CHECK(models[0].colorRamps == model.colorRamps);
    }
    SECTION("malformed image fails closed") {
        models = {model};
        textures = {source};
        textures[0].image.pixels.pop_back();
        CHECK_THROWS_AS(resolveLegendsN64ColorRamps(models, textures, animation), FormatError);
    }
    SECTION("unknown mixed lighting is not silently recolored") {
        models = {model};
        models[0].mesh.prelit = false;
        textures = {source};
        std::vector<std::string> warnings;
        resolveLegendsN64ColorRamps(models, textures, animation, &warnings);
        CHECK(warnings.size() == 1);
        CHECK_FALSE(models[0].mesh.prelit);
        CHECK(textures.size() == 1);
        CHECK(models[0].mesh.vertices == model.mesh.vertices);
        CHECK(models[0].colorRamps == model.colorRamps);
    }
    SECTION("explicitly shadeless models ignore their RSP lighting flag") {
        models = {model};
        models[0].mesh.prelit = false;
        models[0].mesh.parts.resize(1);
        textures = {source};
        resolveLegendsN64ColorRamps(models, textures, animation);
        CHECK(models[0].mesh.prelit);
        CHECK(models[0].mesh.vertices[3].color == Color::white());
        CHECK(textures[1].image.pixel(0, 0) == Color::rgba(128, 16, 0, 7));
    }
}

TEST_CASE("N64 keyed color ramps retain every frame and do not retint shared native slots",
          "[formats][legends][color-ramp]") {
    LegendsN64Model model;
    model.name = "EFFECT";
    model.mesh.prelit = true;
    model.mesh.vertices.resize(3);
    model.mesh.parts = {{0, 0, {0, 1, 2}}};
    model.colorRamps = {{0, Color::rgba(0, 192, 0, 255), Color::rgba(64, 255, 64, 255)}};
    std::vector<LegendsN64Model> models{model};
    std::vector<LegendsN64Texture> textures(3);
    for (usize i = 0; i < textures.size(); ++i) {
        textures[i].name = std::to_string(i);
        textures[i].image = Image::filled(1, 1, i == 1 ? Color::rgba(0, 0, 0, 11) : Color::white());
    }
    AnimationFile animation;
    TextureAnimation motion;
    motion.texture = 0;
    motion.source = 1;
    motion.frames = 2;
    motion.offset = 3;
    motion.rate = 5;
    animation.textureAnimations.push_back(motion);
    TreeNode node;
    node.object = "EFFECT";
    node.textureAnimation = 0;
    TreeDefinition tree;
    tree.nodes.push_back(node);
    animation.trees = {tree, tree};
    SECTION("missing later frame preserves the complete original animation") {
        textures[2].external = true;
        std::vector<std::string> warnings;
        resolveLegendsN64ColorRamps(models, textures, animation, &warnings);
        CHECK(warnings.size() == 1);
        CHECK(textures.size() == 3);
        CHECK(animation.textureAnimations.size() == 1);
        CHECK(animation.trees[0].nodes[0].textureAnimation == 0);
        CHECK(models[0].mesh.vertices == model.mesh.vertices);
        CHECK(models[0].colorRamps == model.colorRamps);
        return;
    }
    SECTION("complete frames share one converted bank") {
        resolveLegendsN64ColorRamps(models, textures, animation);
        REQUIRE(textures.size() == 5); // One shared converted frame block, not one per tree.
        CHECK(textures[1].image.pixel(0, 0) == Color::rgba(0, 0, 0, 11));
        CHECK(textures[3].image.pixel(0, 0) == Color::rgba(0, 192, 0, 11));
        CHECK(textures[4].image.pixel(0, 0) == Color::rgba(64, 255, 64, 255));
        CHECK(models[0].mesh.parts[0].texture == 3);
        for (const auto& converted : animation.trees) {
            REQUIRE(converted.nodes[0].textureAnimation > 0);
            const auto& cycle = animation.textureAnimations[converted.nodes[0].textureAnimation];
            CHECK(cycle.texture == 3);
            CHECK(cycle.source == 3);
            CHECK(cycle.frames == 2);
            CHECK(cycle.offset == motion.offset);
            CHECK(cycle.rate == motion.rate);
        }
    }
}

TEST_CASE("N64 shared color-ramp cycles retain animation and transparent first frames",
          "[formats][legends][color-ramp]") {
    LegendsN64Model model;
    model.name = "ARC";
    model.mesh.prelit = true;
    model.mesh.vertices.resize(3);
    model.mesh.parts = {{0, 0, {0, 1, 2}}};
    model.colorRamps = {{0, Color::rgba(0, 192, 0, 255), Color::rgba(64, 255, 64, 255)}};
    std::vector<LegendsN64Model> models{model, model};
    models[1].name = "SECOND_ARC";
    std::vector<LegendsN64Texture> textures(3);
    for (usize i = 0; i < textures.size(); ++i) {
        textures[i].name = std::to_string(i);
        textures[i].image = Image::filled(1, 1, i < 2 ? Color::rgba(0, 0, 0, 0) : Color::white());
    }
    AnimationFile animation;
    TextureAnimation motion;
    motion.texture = 0;
    motion.source = 1;
    motion.frames = 2;
    motion.rate = 2;
    motion.start = 1;
    motion.flag = -1;
    animation.textureAnimations.push_back(motion);
    SECTION("unavailable later frames preserve the original binding") {
        textures[2].external = true;
        std::vector<std::string> warnings;
        resolveLegendsN64ColorRamps(models, textures, animation, &warnings);
        CHECK(warnings.size() == 1);
        CHECK(textures.size() == 3);
        CHECK(animation.textureAnimations.size() == 1);
        CHECK(models[0].mesh.parts[0].texture == 0);
        CHECK(models[0].colorRamps == model.colorRamps);
    }
    SECTION("shared clocks address the retinted destination, not its original slot") {
        resolveLegendsN64ColorRamps(models, textures, animation);
        REQUIRE(textures.size() == 6);
        REQUIRE(animation.textureAnimations.size() == 2); // Shared across both models.
        const auto& cycle = animation.textureAnimations.back();
        CHECK(cycle.flag == -1);
        CHECK(cycle.texture == 3);
        CHECK(cycle.source == 4);
        CHECK(cycle.frames == 2);
        CHECK(cycle.rate == motion.rate);
        CHECK(cycle.start == motion.start);
        CHECK(models[0].mesh.parts[0].texture == 3);
        CHECK(models[1].mesh.parts[0].texture == 3);
        CHECK(textures[3].image.pixel(0, 0).a == 0);
        CHECK(textures[4].image.pixel(0, 0) == Color::rgba(0, 192, 0, 0));
        CHECK(textures[5].image.pixel(0, 0) == Color::rgba(64, 255, 64, 255));
        CHECK(textures[2].image.pixel(0, 0) == Color::white());
        resolveLegendsN64ColorRamps(models, textures, animation);
        CHECK(textures.size() == 6);
        CHECK(animation.textureAnimations.size() == 2);
    }
}

TEST_CASE("Native barrel fire and poison effects use their authored RDP color ramps",
          "[formats][legends][color-ramp][barrels][native]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "weapons";
    if (!std::filesystem::is_regular_file(directory / "objects.wad")) {
        SKIP("Native N64 Legends weapon artwork unavailable");
    }
    const auto objects = readFile(directory / "objects.wad");
    auto models = decodeLegendsN64Models(objects);
    auto decoded = decodeLegendsN64Sequences(readFile(directory / "seq.wad"), models,
                                             readFile(root / "_program/game.bin"));
    models.insert(models.end(), decoded.modelVariants.begin(), decoded.modelVariants.end());
    auto textures = decodeLegendsN64Textures(objects, readFile(directory / "texdyn.rom"));
    const auto originalCount = textures.size();
    const auto originalFire = textures.at(81).image.pixels;
    REQUIRE(models.at(5).mesh.vertices.front().color == Color::rgba(0, 120, 203, 255));
    REQUIRE(models.at(5).colorRamps.size() == 1);
    REQUIRE(models.at(9).colorRamps.size() == 1);
    CHECK(models.at(5).colorRamps[0].primitive == Color::rgba(128, 16, 0, 255));
    CHECK(models.at(5).colorRamps[0].environment == Color::rgba(255, 255, 0, 255));
    CHECK(models.at(9).colorRamps[0].primitive == Color::rgba(0, 192, 0, 255));
    CHECK(models.at(9).colorRamps[0].environment == Color::rgba(64, 255, 64, 255));
    resolveLegendsN64ColorRamps(models, textures, decoded.animation);
    CHECK(textures.at(81).image.pixels == originalFire);
    for (const char* const sequence : {"WEAPONS:FBAL", "WEAPONS:PBAL"}) {
        CAPTURE(sequence);
        const bool fire = std::string_view(sequence) == "WEAPONS:FBAL";
        const auto tree =
            std::ranges::find(decoded.animation.trees, sequence, &TreeDefinition::name);
        REQUIRE(tree != decoded.animation.trees.end());
        const auto node = std::ranges::find_if(tree->nodes, [](const auto& value) {
            return value.textureAnimation >= 0 && !value.object.empty();
        });
        REQUIRE(node != tree->nodes.end());
        const auto model = std::ranges::find(models, node->object, &LegendsN64Model::name);
        REQUIRE(model != models.end());
        const auto& cycle = decoded.animation.textureAnimations.at(node->textureAnimation);
        REQUIRE(cycle.source >= static_cast<s32>(originalCount));
        CHECK(cycle.frames == (fire ? 15 : 30));
        for (const auto& part : model->mesh.parts) {
            CHECK(part.texture == static_cast<u32>(cycle.texture));
            for (const auto index : part.indices) {
                CHECK(model->mesh.vertices[index].color == Color::white());
            }
        }
        for (s32 frame = 0; frame < cycle.frames; ++frame) {
            const auto& image = textures.at(cycle.source + frame).image;
            REQUIRE_FALSE(image.pixels.empty());
            for (usize pixel = 0; pixel < image.pixels.size(); pixel += 4) {
                if (fire) {
                    CHECK(image.pixels[pixel] >= 128);
                    CHECK(image.pixels[pixel + 2] == 0);
                } else {
                    CHECK(image.pixels[pixel + 1] >= 192);
                    CHECK(image.pixels[pixel] == image.pixels[pixel + 2]);
                }
            }
        }
    }
}

} // namespace
