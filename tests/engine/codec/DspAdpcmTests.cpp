#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/codec/DspAdpcm.h"
#include "engine/core/Error.h"

namespace {

using namespace gdl;

DspAdpcmCoefficients coefficients(s16 first, s16 second) {
    DspAdpcmCoefficients c{};
    c[0] = first;
    c[1] = second;
    c[2] = 2048; // pair 1: previous sample carried forward
    return c;
}

TEST_CASE("with zero predictors the residual nibbles come out scaled", "[codec][adpcm]") {
    DspAdpcmDecoder decoder(coefficients(0, 0));
    // header: pair 0, scale 2^0; nibbles 0,1,2,-1,7,-8,...
    const std::array<u8, 8> kFrame{0x00, 0x01, 0x2F, 0x78, 0x00, 0x00, 0x00, 0x00};
    std::vector<s16> out;
    decoder.decode(kFrame, out);
    REQUIRE(out.size() == 14);
    REQUIRE(out[0] == 0);
    REQUIRE(out[1] == 1);
    REQUIRE(out[2] == 2);
    REQUIRE(out[3] == -1);
    REQUIRE(out[4] == 7);
    REQUIRE(out[5] == -8);
    REQUIRE(out[13] == 0);

    const std::array<u8, 8> kScaled{0x04, 0x10, 0xF0, 0, 0, 0, 0, 0};
    out.clear();
    decoder.decode(kScaled, out);
    REQUIRE(out[0] == 16);
    REQUIRE(out[2] == -16);
}

TEST_CASE("a unit predictor accumulates across frames", "[codec][adpcm]") {
    DspAdpcmDecoder decoder(coefficients(0, 0));
    const std::array<u8, 16> kFrames{0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                                     0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    std::vector<s16> out;
    decoder.decode(kFrames, out);
    REQUIRE(out.size() == 28);
    REQUIRE(out[0] == 1);
    REQUIRE(out[13] == 14);
    REQUIRE(out[27] == 28);
    decoder.reset();
    out.clear();
    decoder.decode(std::span(kFrames).subspan(0, 8), out);
    REQUIRE(out[0] == 1);
}

TEST_CASE("samples clamp at the 16-bit limits", "[codec][adpcm]") {
    DspAdpcmDecoder decoder(coefficients(0, 0));
    const std::array<u8, 8> kLoud{0x0F, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
    std::vector<s16> out;
    decoder.decode(kLoud, out);
    REQUIRE(out[0] == 32767);
    const std::array<u8, 8> kQuiet{0x0F, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88};
    out.clear();
    decoder.decode(kQuiet, out);
    REQUIRE(out[0] == -32768);
}

TEST_CASE("coefficient tables are big-endian and partial frames are rejected", "[codec][adpcm]") {
    std::array<u8, 32> bytes{};
    bytes[0] = 0x09;
    bytes[1] = 0x90;
    bytes[2] = 0xFA;
    bytes[3] = 0x06;
    const DspAdpcmCoefficients table = readDspAdpcmCoefficients(bytes);
    REQUIRE(table[0] == 0x0990);
    REQUIRE(table[1] == -1530);
    REQUIRE_THROWS_AS(readDspAdpcmCoefficients(std::span(bytes).subspan(0, 10)), FormatError);

    DspAdpcmDecoder decoder(table);
    std::vector<s16> out;
    REQUIRE_THROWS_AS(decoder.decode(std::span(bytes).subspan(0, 12), out), FormatError);
}

} // namespace
