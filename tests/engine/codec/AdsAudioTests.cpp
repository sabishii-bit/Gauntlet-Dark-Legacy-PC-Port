#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/codec/AdsAudio.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"

namespace {

using namespace gdl;
using Catch::Matchers::WithinAbs;

void putBigU32(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value >> 24U));
    out.push_back(static_cast<u8>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<u8>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<u8>(value & 0xFFU));
}

/** Header for a stream whose channels all use zero predictors. */
std::vector<u8> header(u32 channels, u32 blockSize, u32 bodySize) {
    std::vector<u8> bytes;
    for (const char c : std::string_view("dhSS")) {
        bytes.push_back(static_cast<u8>(c));
    }
    putBigU32(bytes, 24);
    putBigU32(bytes, 32);
    putBigU32(bytes, 48000);
    putBigU32(bytes, channels);
    putBigU32(bytes, blockSize);
    putBigU32(bytes, 0xFFFFFFFFU);
    putBigU32(bytes, 0);
    for (const char c : std::string_view("dbSS")) {
        bytes.push_back(static_cast<u8>(c));
    }
    putBigU32(bytes, bodySize);
    for (u32 c = 0; c < channels; ++c) {
        std::vector<u8> channelHeader(96, 0);
        channelHeader[3] = 28; // sample count
        bytes.insert(bytes.end(), channelHeader.begin(), channelHeader.end());
    }
    return bytes;
}

TEST_CASE("the header is recognised and parsed", "[codec][ads]") {
    const std::vector<u8> bytes = header(2, 32, 64);
    REQUIRE(AdsAudioDecoder::looksLikeAds(bytes));
    REQUIRE_FALSE(AdsAudioDecoder::looksLikeAds(std::vector<u8>{1, 2, 3, 4}));
    REQUIRE(AdsAudioDecoder::headerSize(2) == 232);

    AdsAudioDecoder decoder;
    REQUIRE_FALSE(decoder.parseHeader(std::span(bytes).subspan(0, 100)).has_value());
    const auto used = decoder.parseHeader(bytes);
    REQUIRE(used.has_value());
    REQUIRE(*used == 232);
    REQUIRE(decoder.hasHeader());
    REQUIRE(decoder.info().sampleRate == 48000);
    REQUIRE(decoder.info().channels == 2);
    REQUIRE(decoder.info().blockSize == 32);
    REQUIRE(decoder.info().bodySize == 64);
    REQUIRE(decoder.info().sampleCount == 28);
}

TEST_CASE("interleaved blocks decode into interleaved samples", "[codec][ads]") {
    AdsAudioDecoder decoder;
    REQUIRE(decoder.parseHeader(header(2, 8, 32)).has_value());

    // left frames carry residual +1 per sample, right frames -2 (scale 0, zero predictors)
    std::vector<u8> body;
    const std::vector<u8> left{0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    const std::vector<u8> right{0x00, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    body.insert(body.end(), left.begin(), left.end());
    body.insert(body.end(), right.begin(), right.end());
    body.insert(body.end(), left.begin(), left.end());
    body.insert(body.end(), right.begin(), right.end());

    std::vector<f32> out;
    decoder.feed(std::span(body).subspan(0, 20), out);
    REQUIRE(out.size() == 28);
    decoder.feed(std::span(body).subspan(20), out);
    REQUIRE(out.size() == 56);
    CHECK_THAT(out[0], WithinAbs(1.0 / 32768.0, 1e-9));
    CHECK_THAT(out[1], WithinAbs(-2.0 / 32768.0, 1e-9));
    CHECK_THAT(out[54], WithinAbs(1.0 / 32768.0, 1e-9));
    CHECK_THAT(out[55], WithinAbs(-2.0 / 32768.0, 1e-9));

    decoder.feed(std::span(left).subspan(0, 4), out);
    REQUIRE(out.size() == 56);
    decoder.flush(out);
    REQUIRE(out.size() == 84);
}

TEST_CASE("bad streams are rejected", "[codec][ads]") {
    AdsAudioDecoder decoder;
    std::vector<f32> out;
    REQUIRE_THROWS_AS(decoder.feed(std::vector<u8>(16, 0), out), FormatError);
    std::vector<u8> wrongFormat = header(1, 8, 8);
    wrongFormat[11] = 16;
    REQUIRE_THROWS_AS(decoder.parseHeader(wrongFormat), FormatError);
    std::vector<u8> notAds = header(1, 8, 8);
    notAds[0] = 'X';
    REQUIRE_THROWS_AS(decoder.parseHeader(notAds), FormatError);
}

} // namespace
