#include <array>
#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/ByteReader.h"

#include "formats/WavWriter.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

TEST_CASE("wave files carry a canonical 44-byte header", "[formats][wav]") {
    const std::array<std::int16_t, 4> kSamples{0, 1000, -1000, 32767};
    const std::vector<std::uint8_t> wav = encodeWav(kSamples, 22050, 2);
    REQUIRE(wav.size() == 44 + 8);
    REQUIRE(std::string(wav.begin(), wav.begin() + 4) == "RIFF");
    REQUIRE(readU32LE(wav, 4) == 36 + 8);
    REQUIRE(std::string(wav.begin() + 8, wav.begin() + 12) == "WAVE");
    REQUIRE(readU32LE(wav, 16) == 16);
    REQUIRE(readU16LE(wav, 20) == 1);
    REQUIRE(readU16LE(wav, 22) == 2);
    REQUIRE(readU32LE(wav, 24) == 22050);
    REQUIRE(readU32LE(wav, 28) == 22050 * 4);
    REQUIRE(readU16LE(wav, 32) == 4);
    REQUIRE(readU16LE(wav, 34) == 16);
    REQUIRE(std::string(wav.begin() + 36, wav.begin() + 40) == "data");
    REQUIRE(readU32LE(wav, 40) == 8);
    REQUIRE(static_cast<std::int16_t>(readU16LE(wav, 46)) == 1000);
    REQUIRE(static_cast<std::int16_t>(readU16LE(wav, 48)) == -1000);
}

} // namespace
