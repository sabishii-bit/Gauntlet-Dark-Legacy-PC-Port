#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WavFile.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/WavWriter.h"

namespace {

using namespace gdl;

TEST_CASE("16-bit PCM wave files round-trip", "[assets][wav]") {
    const std::array<s16, 6> kSamples{1, -1, 2000, -2000, 32767, -32768};
    const WavData wav = decodeWav(formats::encodeWav(kSamples, 12000, 2));
    REQUIRE(wav.sampleRate == 12000);
    REQUIRE(wav.channels == 2);
    REQUIRE(wav.frames() == 3);
    REQUIRE(wav.samples == std::vector<s16>(kSamples.begin(), kSamples.end()));
}

TEST_CASE("wave files load from disk and bad ones are rejected", "[assets][wav]") {
    const auto dir = test::scratchDirectory("wav-file");
    const std::array<s16, 2> kSamples{5, 6};
    writeFile(dir / "a.wav", formats::encodeWav(kSamples, 8000, 1));
    REQUIRE(loadWav(dir / "a.wav").samples.size() == 2);
    REQUIRE_THROWS_AS(loadWav(dir / "missing.wav"), FileError);
    REQUIRE_THROWS_AS(decodeWav(std::vector<u8>(20, 0)), FormatError);
    std::vector<u8> eightBit = formats::encodeWav(kSamples, 8000, 1);
    eightBit[34] = 8;
    REQUIRE_THROWS_AS(decodeWav(eightBit), FormatError);
}

} // namespace
