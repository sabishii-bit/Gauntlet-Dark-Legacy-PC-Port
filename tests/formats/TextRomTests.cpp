#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/TextRom.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using test::ByteWriter;

constexpr u32 lumpId(std::string_view code) {
    return (u32{static_cast<u8>(code[0])} << 24U) | (u32{static_cast<u8>(code[1])} << 16U) |
           (u32{static_cast<u8>(code[2])} << 8U) | u32{static_cast<u8>(code[3])};
}

/** Two fonts, three strings, two messages (one with two lines) and one list. */
std::vector<u8> sampleRom() {
    ByteWriter body;
    body.putU32(0).putU32(0); // header placeholder

    const u32 fontsAt = 8;
    body.putText("small").putZeros(11).putU32(0);
    body.putText("big").putZeros(13).putU32(1);

    const u32 textAt = static_cast<u32>(body.size());
    body.putText("Hello").putU8(0).putText("World").putU8(0).putText("Bye").putU8(0);
    body.putZeros(2);
    const u32 toffAt = static_cast<u32>(body.size());
    body.putU32(0).putU32(6).putU32(12);
    const u32 strsAt = static_cast<u32>(body.size());
    body.putS32(2).putS32(0).putS32(1).putU32(0x3F800000).putU32(0x3F000000);
    body.putS32(1).putS32(2).putS32(0).putU32(0x40000000).putU32(0x3F800000);
    const u32 loffAt = static_cast<u32>(body.size());
    body.putU32(1).putU32(0);
    const u32 listAt = static_cast<u32>(body.size());
    body.putS32(2).putS32(0);
    const u32 defsAt = static_cast<u32>(body.size());
    body.putText("greeting").putU8(0).putText("farewell").putU8(0).putText("all").putU8(0);
    body.putZeros(1);
    const u32 sdefAt = static_cast<u32>(body.size());
    body.putU32(0).putU32(9);
    const u32 ldefAt = static_cast<u32>(body.size());
    body.putU32(18);

    const u32 tableAt = static_cast<u32>(body.size());
    const auto lump = [&body](std::string_view id, u32 at, u32 count) {
        body.putU32(lumpId(id)).putU32(at).putU32(count).putU32(0);
    };
    lump("FONT", fontsAt, 2);
    lump("TEXT", textAt, 1);
    lump("TOFF", toffAt, 1);
    lump("STRS", strsAt, 2);
    lump("LOFF", loffAt, 1);
    lump("LIST", listAt, 1);
    lump("DEFS", defsAt, 1);
    lump("SDEF", sdefAt, 2);
    lump("LDEF", ldefAt, 1);

    std::vector<u8> bytes = body.bytes();
    bytes[0] = static_cast<u8>(tableAt & 0xFFU);
    bytes[1] = static_cast<u8>((tableAt >> 8U) & 0xFFU);
    bytes[4] = 9;
    return bytes;
}

TEST_CASE("a synthetic text rom parses fonts, messages and lists", "[formats][text]") {
    const TextRom rom = TextRom::parse(sampleRom());
    REQUIRE(rom.fonts == std::vector<std::string>{"small", "big"});
    REQUIRE(rom.messages.size() == 2);
    REQUIRE(rom.messages[0].name == "GREETING");
    REQUIRE(rom.messages[0].font == 1);
    REQUIRE(rom.messages[0].scale == 1.0f);
    REQUIRE(rom.messages[0].shadowScale == 0.5f);
    REQUIRE(rom.messages[0].lines == std::vector<std::string>{"Hello", "World"});
    REQUIRE(rom.messages[1].name == "FAREWELL");
    REQUIRE(rom.messages[1].scale == 2.0f);
    REQUIRE(rom.messages[1].lines == std::vector<std::string>{"Bye"});
    REQUIRE(rom.lists.size() == 1);
    REQUIRE(rom.lists[0].name == "ALL");
    REQUIRE(rom.lists[0].messages == std::vector<u32>{1, 0});
    REQUIRE(rom.findMessage("farewell") == 1U);
    REQUIRE(rom.findList("ALL") == 0U);
    REQUIRE_FALSE(rom.findMessage("nothing").has_value());
}

TEST_CASE("damaged text roms are rejected", "[formats][text]") {
    REQUIRE_THROWS_AS(TextRom::parse(std::vector<u8>(4, 0)), FormatError);
    std::vector<u8> bad = sampleRom();
    bad[0] = 0xFF;
    bad[1] = 0xFF;
    REQUIRE_THROWS_AS(TextRom::parse(bad), FormatError);
}

TEST_CASE("the English text rom carries the menu fonts and named messages",
          "[formats][text][assets]") {
    const auto file = test::assetOrSkip("TEXT/ENGLISH.ROM");
    const TextRom rom = TextRom::parse(readFile(file));
    REQUIRE(rom.fonts == std::vector<std::string>{"8Hi_fonts5", "font32", "initials"});
    REQUIRE(rom.messages.size() == 205);
    REQUIRE(rom.lists.size() == 4);
    std::set<std::string> names;
    for (const TextMessage& message : rom.messages) {
        REQUIRE_FALSE(message.name.empty());
        names.insert(message.name);
    }
    REQUIRE(names.size() > 150);
}

} // namespace
