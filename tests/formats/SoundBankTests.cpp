#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/SoundBank.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

class BigEndianWriter {
public:
    BigEndianWriter& put16(u16 value) {
        m_bytes.push_back(static_cast<u8>(value >> 8U));
        m_bytes.push_back(static_cast<u8>(value & 0xFFU));
        return *this;
    }
    BigEndianWriter& put32(u32 value) {
        put16(static_cast<u16>(value >> 16U));
        return put16(static_cast<u16>(value & 0xFFFFU));
    }
    BigEndianWriter& text(std::string_view s, usize width) {
        for (usize i = 0; i < width; ++i) {
            m_bytes.push_back(i < s.size() ? static_cast<u8>(s[i]) : u8{0});
        }
        return *this;
    }
    BigEndianWriter& zeros(usize count) {
        m_bytes.insert(m_bytes.end(), count, u8{0});
        return *this;
    }
    BigEndianWriter& bytes(std::span<const u8> data) {
        m_bytes.insert(m_bytes.end(), data.begin(), data.end());
        return *this;
    }
    const std::vector<u8>& get() const { return m_bytes; }

private:
    std::vector<u8> m_bytes;
};

/** One silent 8-byte ADPCM frame with coefficient pair 0 and scale 0. */
constexpr std::array<u8, 8> kSilentFrame{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/** Two calls (one with a looping two-sample sequence) and one sample. */
std::vector<u8> sampleBank() {
    BigEndianWriter calls;
    calls.put16(0x8000).put16(0x7F).put16(0).put16(0x8001); // call 0: sample 0, vol 127
    calls.put16(0x4001).put16(0x0000).put16(0xA001).put16(0x6E).put16(3).put16(
        0x0009); // call 1: loop
    BigEndianWriter w;
    w.put32(0x4B4E4256)
        .put32(static_cast<u32>(calls.get().size()))
        .put32(0x01070106)
        .put32(2)
        .put32(1);
    w.bytes(calls.get());
    w.put32(0x56414770).put32(0x28).put32(0).put32(16).put32(12000);
    w.zeros(12).text("blip", 16);
    // DSP header: samples, nibbles, rate, loop flag, format, loop start, loop end, ca, coefs...
    w.put32(28).put32(32).put32(12000).put16(1).put16(0).put32(2).put32(28).put32(2);
    for (s16 i = 0; i < 16; ++i) {
        w.put16(static_cast<u16>(i * 100));
    }
    w.zeros(96 - 28 - 32);
    w.bytes(kSilentFrame).bytes(kSilentFrame);
    return w.get();
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
