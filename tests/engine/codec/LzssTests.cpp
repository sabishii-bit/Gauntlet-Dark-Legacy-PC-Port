#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/codec/Lzss.h"
#include "engine/core/Error.h"

namespace {

using namespace gdl;

TEST_CASE("stored blocks are copied verbatim", "[codec][lzss]") {
    const std::vector<u8> packed{1, 0, 0, 0, 'a', 'b', 'c'};
    REQUIRE(lzssUnpack(packed) == std::vector<u8>{'a', 'b', 'c'});
}

TEST_CASE("matches copy from earlier output, overlapping allowed", "[codec][lzss]") {
    // flag word 0x0004: tokens 0 and 1 literal, token 2 a match of distance 2, length 3
    const std::vector<u8> packed{0, 0, 0, 0, 0x04, 0x00, 'a', 'b', 0x00, 0x02};
    REQUIRE(lzssUnpack(packed) == std::vector<u8>{'a', 'b', 'a', 'b', 'a'});
}

TEST_CASE("long distances use the high nibble of the first match byte", "[codec][lzss]") {
    std::vector<u8> packed{0, 0, 0, 0};
    std::vector<u8> expected;
    // 16 literals in one flag word, then a second word whose first token is a match
    packed.push_back(0x00);
    packed.push_back(0x00);
    for (u8 i = 0; i < 16; ++i) {
        packed.push_back(i);
        expected.push_back(i);
    }
    packed.push_back(0x01);
    packed.push_back(0x00);
    packed.push_back(0x00 | 0x02); // distance high nibble 0, length 5
    packed.push_back(0x10);        // distance 16
    for (u8 i = 0; i < 5; ++i) {
        expected.push_back(i);
    }
    REQUIRE(lzssUnpack(packed) == expected);
}

TEST_CASE("malformed streams throw FormatError", "[codec][lzss]") {
    REQUIRE_THROWS_AS(lzssUnpack(std::vector<u8>{0, 0}), FormatError);
    REQUIRE_THROWS_AS(lzssUnpack(std::vector<u8>{0, 0, 0, 0, 0x01}), FormatError);
    REQUIRE_THROWS_AS(lzssUnpack(std::vector<u8>{0, 0, 0, 0, 0x01, 0x00, 0x00, 0x05}), FormatError);
    REQUIRE_THROWS_AS(lzssUnpack(std::vector<u8>{0, 0, 0, 0, 0x01, 0x00, 0x00, 0x00}), FormatError);
}

} // namespace
