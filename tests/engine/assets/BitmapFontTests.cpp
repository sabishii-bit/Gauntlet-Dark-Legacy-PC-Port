#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

TEST_CASE("glyphs are found by character code", "[assets][font]") {
    const BitmapFont font =
        BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}, {'A', 1, 1, 1}});
    REQUIRE(font.loaded());
    REQUIRE(font.height() == 10);
    REQUIRE(font.spaceWidth() == 4);
    REQUIRE(font.glyphCount() == 3);
    REQUIRE(font.glyph('A') != nullptr);
    REQUIRE(font.glyph('A')->width == 6);
    REQUIRE(font.glyph('B')->x == 6);
    REQUIRE(font.glyph('C') == nullptr);
    REQUIRE(font.glyph(0) == nullptr);
}

TEST_CASE("font manifests load from disk", "[assets][font]") {
    const auto dir = test::scratchDirectory("bitmap-font");
    writeTextFile(dir / "small.json",
                  R"({"height": 12, "glyphs": [{"code": 65, "width": 7, "x": 10, "y": 20}]})");
    BitmapFont font;
    REQUIRE(font.load(dir / "small.json", 5));
    REQUIRE(font.height() == 12);
    REQUIRE(font.spaceWidth() == 5);
    REQUIRE(font.glyph('A')->y == 20);

    writeTextFile(dir / "bad.json", R"({"height": 0, "glyphs": []})");
    REQUIRE_FALSE(font.load(dir / "bad.json", 5));
    REQUIRE_FALSE(font.loaded());
    REQUIRE_FALSE(font.load(dir / "missing.json", 5));
}

TEST_CASE("the unpacked menu font has 32 pixel glyphs", "[assets][font][unpacked]") {
    const auto file = test::unpackedOrSkip("fonts/font32.json");
    BitmapFont font;
    REQUIRE(font.load(file, 16));
    REQUIRE(font.height() == 32);
    REQUIRE(font.glyph('P') != nullptr);
    REQUIRE(font.glyph('P')->width == 24);
    REQUIRE(font.glyph(' ') == nullptr);
}

} // namespace
