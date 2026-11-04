#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/SoundBank.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using test::SyntheticBytes;

/** The fixed name field of a sample's header. */
constexpr usize kNameSize = 16;

/** One silent 8-byte ADPCM frame with coefficient pair 0 and scale 0. */
constexpr std::array<u8, 8> kSilentFrame{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/** Two calls (one with a looping two-sample sequence) and one sample. */
std::vector<u8> sampleBank() {
    SyntheticBytes calls;
    calls.putU16BE(0x8000).putU16BE(0x7F).putU16BE(0).putU16BE(0x8001); // call 0: sample 0, vol 127
    calls.putU16BE(0x4001).putU16BE(0x0000).putU16BE(0xA001).putU16BE(0x6E).putU16BE(3).putU16BE(
        0x0009); // call 1: loop
    SyntheticBytes w;
    w.putU32BE(0x4B4E4256)
        .putU32BE(static_cast<u32>(calls.size()))
        .putU32BE(0x01070106)
        .putU32BE(2)
        .putU32BE(1);
    w.putBytes(calls.bytes());
    w.putU32BE(0x56414770).putU32BE(0x28).putU32BE(0).putU32BE(16).putU32BE(12000);
    constexpr std::string_view kSampleName = "blip";
    w.putZeros(12).putText(kSampleName).putZeros(kNameSize - kSampleName.size());
    // DSP header: samples, nibbles, rate, loop flag, format, loop start, loop end, ca, coefs...
    w.putU32BE(28)
        .putU32BE(32)
        .putU32BE(12000)
        .putU16BE(1)
        .putU16BE(0)
        .putU32BE(2)
        .putU32BE(28)
        .putU32BE(2);
    for (s16 i = 0; i < 16; ++i) {
        w.putU16BE(static_cast<u16>(i * 100));
    }
    w.putZeros(96 - 28 - 32);
    w.putBytes(kSilentFrame).putBytes(kSilentFrame);
    return w.bytes();
}

TEST_CASE("a sound bank parses calls with their sequences and parameters", "[formats][sound]") {
    const SoundBank bank = SoundBank::parse(sampleBank());
    REQUIRE(bank.calls.size() == 2);
    REQUIRE(bank.samples.size() == 1);

    const SoundCall& first = bank.calls[0];
    REQUIRE(first.steps.size() == 1);
    REQUIRE(first.steps[0].sample == 0);
    REQUIRE_FALSE(first.steps[0].loopStart);
    REQUIRE(first.volume == 127);
    REQUIRE(first.duck == 0);
    REQUIRE(first.priority == 0x8001);

    const SoundCall& music = bank.calls[1];
    REQUIRE(music.steps.size() == 3);
    REQUIRE(music.steps[0].sample == 1);
    REQUIRE(music.steps[0].loopStart);
    REQUIRE(music.steps[2].sample == 1);
    REQUIRE(music.steps[2].loopBack);
    REQUIRE(music.volume == 110);
    REQUIRE(music.duck == 3);

    const BankSample& sample = bank.samples[0];
    REQUIRE(sample.name == "blip");
    REQUIRE(sample.sampleRate == 12000);
    REQUIRE(sample.sampleCount == 28);
    REQUIRE(sample.loops);
    REQUIRE(sample.loopStart == 2);
    REQUIRE(sample.loopEnd == 28);
    REQUIRE(sample.coefficients[1] == 100);
    REQUIRE(sample.adpcm.size() == 16);

    const std::vector<s16> pcm = decodeBankSample(sample);
    REQUIRE(pcm.size() == 28);
    REQUIRE(pcm[0] == 0);
}

TEST_CASE("damaged sound banks are rejected", "[formats][sound]") {
    REQUIRE_THROWS_AS(SoundBank::parse(std::vector<u8>(8, 0)), FormatError);
    std::vector<u8> wrongMagic = sampleBank();
    wrongMagic[0] = 'X';
    REQUIRE_THROWS_AS(SoundBank::parse(wrongMagic), FormatError);
    std::vector<u8> truncated = sampleBank();
    truncated.resize(truncated.size() - 4);
    REQUIRE_THROWS_AS(SoundBank::parse(truncated), FormatError);
}

TEST_CASE("the shipped common bank holds the menu blips", "[formats][sound][assets]") {
    const auto file = test::assetOrSkip("AUDIO/COMMON.VBK");
    const SoundBank bank = SoundBank::parse(readFile(file));
    REQUIRE(bank.calls.size() == 105);
    REQUIRE(bank.samples.size() == 106);
    const SoundCall& move = bank.calls[13];
    REQUIRE(move.steps.size() == 1);
    REQUIRE(move.volume <= SoundBank::kMaxVolume);
    const BankSample& sample = bank.samples[move.steps[0].sample];
    REQUIRE(sample.sampleRate > 0);
    const std::vector<s16> pcm = decodeBankSample(sample);
    REQUIRE(pcm.size() == sample.sampleCount);
    REQUIRE(pcm.size() > 100);
}

TEST_CASE("the select bank's music is a looping sequence", "[formats][sound][assets]") {
    const auto file = test::assetOrSkip("AUDIO/SELECT.VBK");
    const SoundBank bank = SoundBank::parse(readFile(file));
    REQUIRE(bank.calls.size() == 134);
    REQUIRE(bank.samples.size() == 55);
    const SoundCall& music = bank.calls[0];
    REQUIRE(music.steps.size() == 6);
    REQUIRE(music.steps[0].loopStart);
    REQUIRE(music.steps[5].loopBack);
    REQUIRE(music.volume == 110);
}

} // namespace
