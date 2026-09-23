#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/FontFile.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using test::ByteWriter;

TEST_CASE("font files list glyph cells until a zero code", "[formats][font]") {
    ByteWriter w;
    w.putU32(0).putS32(12).putU32(0);
    w.putS32('A').putS32(7).putS32(10).putS32(20);
    w.putS32('B').putS32(8).putS32(17).putS32(20);
    w.putS32(0).putS32(0).putS32(0).putS32(0);
    w.putS32('Z').putS32(1).putS32(1).putS32(1);
    const FontFile font = FontFile::parse(w.bytes());
    REQUIRE(font.height == 12);
    REQUIRE(font.glyphs.size() == 2);
    REQUIRE(font.glyphs[1].code == 'B');
    REQUIRE(font.glyphs[1].width == 8);
    REQUIRE(font.glyphs[1].x == 17);
    REQUIRE(font.glyphs[1].y == 20);

    REQUIRE_THROWS_AS(FontFile::parse(std::vector<u8>(8, 0)), FormatError);
    ByteWriter zeroHeight;
    zeroHeight.putU32(0).putS32(0).putU32(0);
    REQUIRE_THROWS_AS(FontFile::parse(zeroHeight.bytes()), FormatError);
}

TEST_CASE("the gaunt font has ten-pixel glyphs", "[formats][font][assets]") {
    const auto file = test::assetOrSkip("FONTS/GAUNT.FNT");
    const FontFile font = FontFile::parse(readFile(file));
    REQUIRE(font.height == 10);
    REQUIRE(font.glyphs.size() == 32);
    REQUIRE(font.glyphs[0].code == '!');
    REQUIRE(font.glyphs[0].width == 6);
}

} // namespace
