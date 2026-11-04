#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "SyntheticBytes.h"
#include "TestSupport.h"

namespace {

using namespace gdl;
using test::SyntheticBytes;

TEST_CASE("synthetic bytes append little-endian values unless a writer says big-endian",
          "[test-support]") {
    SyntheticBytes bytes;
    bytes.putU8(0x01)
        .putU16(0x0302)
        .putU32(0x07060504)
        .putS32(-2)
        .putF32(1.0f)
        .putU16BE(0x0809)
        .putU32BE(0x0A0B0C0D)
        .putFourcc("RIFF")
        .putText("ab")
        .putZeros(2)
        .putBytes(std::vector<u8>{0xEE, 0xFF});
    constexpr std::string_view kExpected = "01"       // u8
                                           "0203"     // u16
                                           "04050607" // u32
                                           "feffffff" // s32 -2
                                           "0000803f" // f32 1.0
                                           "0809"     // u16, big-endian
                                           "0a0b0c0d" // u32, big-endian
                                           "52494646" // the four characters in order
                                           "6162"     // text, no terminator
                                           "0000"     // zeros
                                           "eeff";    // bytes as given
    CHECK(test::hexBytes(bytes.bytes()) == kExpected);
    CHECK(bytes.size() == 31);
}

TEST_CASE("field writers set each value at its offset in the order their names give",
          "[test-support]") {
    std::vector<u8> bytes(24, 0xAA);
    test::setU16LE(bytes, 0, 0x0201);
    test::setU32LE(bytes, 2, 0x06050403);
    test::setF32LE(bytes, 6, -2.0f);
    test::setU16BE(bytes, 10, 0x0B0C);
    test::setU32BE(bytes, 12, 0x0D0E0F10);
    test::setF32BE(bytes, 16, 0.5f);
    test::setText(bytes, 20, 3, "hi");
    constexpr std::string_view kExpected = "0102"     // u16
                                           "03040506" // u32
                                           "000000c0" // f32 -2.0
                                           "0b0c"     // u16, big-endian
                                           "0d0e0f10" // u32, big-endian
                                           "3f000000" // f32 0.5, big-endian
                                           "686900"   // text, the rest of its field zeroed
                                           "aa";      // past every field, untouched
    CHECK(test::hexBytes(bytes) == kExpected);
}

TEST_CASE("a field past the end of its bytes, or text longer than its field, throws unwritten",
          "[test-support]") {
    std::vector<u8> bytes(24, 0xAA);
    const std::vector<u8> before = bytes;
    REQUIRE_THROWS_AS(test::setU32LE(bytes, 21, 0), std::out_of_range);
    REQUIRE_THROWS_AS(test::setU32BE(bytes, 21, 0), std::out_of_range);
    REQUIRE_THROWS_AS(test::setF32LE(bytes, 22, 0.0f), std::out_of_range);
    REQUIRE_THROWS_AS(test::setU16LE(bytes, 23, 0), std::out_of_range);
    REQUIRE_THROWS_AS(test::setU16BE(bytes, 25, 0), std::out_of_range);
    REQUIRE_THROWS_AS(test::setU32LE(bytes, std::numeric_limits<usize>::max(), 0),
                      std::out_of_range);
    REQUIRE_THROWS_AS(test::setText(bytes, 22, 4, "ab"), std::out_of_range);
    REQUIRE_THROWS_AS(test::setText(bytes, 0, 2, "abc"), std::length_error);
    CHECK(bytes == before);

    REQUIRE_NOTHROW(test::setU16LE(bytes, 22, 0));
    REQUIRE_NOTHROW(test::setText(bytes, 0, 2, "ab"));
    CHECK(test::hexBytes(bytes) == "6162aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa0000");
}

} // namespace
