#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace {

using namespace gdl;

TEST_CASE("fourcc packs characters low byte first", "[io][bytes]") {
    STATIC_REQUIRE(fourcc("RIFF") == 0x46464952U);
    STATIC_REQUIRE(fourcc("00dc") == 0x63643030U);
}

TEST_CASE("little-endian span readers", "[io][bytes]") {
    constexpr std::array<std::uint8_t, 6> kBytes{0x34, 0x12, 0x78, 0x56, 0x34, 0x12};
    STATIC_REQUIRE(readU16LE(kBytes, 0) == 0x1234);
    STATIC_REQUIRE(readU32LE(kBytes, 2) == 0x12345678U);
    constexpr std::array<std::uint8_t, 4> kNegative{0xFF, 0xFF, 0xFF, 0xFF};
    STATIC_REQUIRE(readS32LE(kNegative, 0) == -1);
}

TEST_CASE("ByteReader walks a buffer and refuses to overrun", "[io][bytes]") {
    constexpr std::array<std::uint8_t, 8> kBytes{1, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ByteReader reader(kBytes);
    REQUIRE(reader.readU8() == 1);
    REQUIRE(reader.readU16() == 0x0302);
    REQUIRE(reader.readU32() == 0x07060504U);
    REQUIRE(reader.remaining() == 1);
    REQUIRE_FALSE(reader.atEnd());
    REQUIRE_THROWS_AS(reader.readU16(), FormatError);
    reader.seek(6);
    const auto tail = reader.readBytes(2);
    REQUIRE(tail.size() == 2);
    REQUIRE(tail[1] == 0x08);
    REQUIRE(reader.atEnd());
    REQUIRE_THROWS_AS(reader.seek(9), FormatError);
    REQUIRE_THROWS_AS(reader.skip(1), FormatError);
}

} // namespace
