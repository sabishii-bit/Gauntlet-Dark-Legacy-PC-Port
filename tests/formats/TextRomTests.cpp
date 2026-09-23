#include <cstdint>
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

constexpr std::uint32_t lumpId(std::string_view code) {
    return (std::uint32_t{static_cast<std::uint8_t>(code[0])} << 24U) |
           (std::uint32_t{static_cast<std::uint8_t>(code[1])} << 16U) |
           (std::uint32_t{static_cast<std::uint8_t>(code[2])} << 8U) |
           std::uint32_t{static_cast<std::uint8_t>(code[3])};
}

/** Two fonts, three strings, two messages (one with two lines) and one list. */
std::vector<std::uint8_t> sampleRom() {
    ByteWriter body;
    body.putU32(0).putU32(0); // header placeholder

    const std::uint32_t fontsAt = 8;
    body.putText("small").putZeros(11).putU32(0);
    body.putText("big").putZeros(13).putU32(1);

    const auto textAt = static_cast<std::uint32_t>(body.size());
    body.putText("Hello").putU8(0).putText("World").putU8(0).putText("Bye").putU8(0);
    body.putZeros(2);
    const auto toffAt = static_cast<std::uint32_t>(body.size());
    body.putU32(0).putU32(6).putU32(12);
    const auto strsAt = static_cast<std::uint32_t>(body.size());
    body.putS32(2).putS32(0).putS32(1).putU32(0x3F800000).putU32(0x3F000000);
    body.putS32(1).putS32(2).putS32(0).putU32(0x40000000).putU32(0x3F800000);
    const auto loffAt = static_cast<std::uint32_t>(body.size());
    body.putU32(1).putU32(0);
    const auto listAt = static_cast<std::uint32_t>(body.size());
    body.putS32(2).putS32(0);
    const auto defsAt = static_cast<std::uint32_t>(body.size());
    body.putText("greeting").putU8(0).putText("farewell").putU8(0).putText("all").putU8(0);
    body.putZeros(1);
    const auto sdefAt = static_cast<std::uint32_t>(body.size());
    body.putU32(0).putU32(9);
    const auto ldefAt = static_cast<std::uint32_t>(body.size());
    body.putU32(18);

    const auto tableAt = static_cast<std::uint32_t>(body.size());
    const auto lump = [&body](std::string_view id, std::uint32_t at, std::uint32_t count) {
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

    std::vector<std::uint8_t> bytes = body.bytes();
    bytes[0] = static_cast<std::uint8_t>(tableAt & 0xFFU);
    bytes[1] = static_cast<std::uint8_t>((tableAt >> 8U) & 0xFFU);
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
    REQUIRE(rom.lists[0].messages == std::vector<std::uint32_t>{1, 0});
    REQUIRE(rom.findMessage("farewell") == 1U);
    REQUIRE(rom.findList("ALL") == 0U);
    REQUIRE_FALSE(rom.findMessage("nothing").has_value());
}

TEST_CASE("damaged text roms are rejected", "[formats][text]") {
    REQUIRE_THROWS_AS(TextRom::parse(std::vector<std::uint8_t>(4, 0)), FormatError);
    std::vector<std::uint8_t> bad = sampleRom();
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
