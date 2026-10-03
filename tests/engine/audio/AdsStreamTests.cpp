#include <array>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AdsStream.h"
#include "engine/audio/AudioStream.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "fixtures/ReferenceWav.h"

namespace {

using namespace gdl;
using Catch::Approx;

TEST_CASE("music discards its transport trailer before ending or rewinding",
          "[audio][stream][ads-trailer]") {
    const auto dir = test::scratchDirectory("ads-trailer");
    std::vector<u8> bytes(AdsAudioDecoder::headerSize(2), 0);
    const auto put = [&](usize at, u32 value) {
        for (usize i = 0; i < 4; ++i) {
            bytes[at + i] = static_cast<u8>(value >> ((3 - i) * 8));
        }
    };
    put(0, 0x64685353); // dhSS
    put(4, 24);
    put(8, 32);
    put(12, 24000);
    put(16, 2);
    put(20, 8);
    put(32, 0x64625353); // dbSS
    put(36, 32);
    put(40, 28);
    put(136, 28);
    for (const u8 residual : std::array<u8, 4>{0x11, 0x22, 0x77, 0x77}) {
        bytes.push_back(0);
        bytes.insert(bytes.end(), 7, residual);
    }
    writeFile(dir / "music.ads", bytes);
    AdsStream stream;
    REQUIRE(stream.open(dir / "music.ads"));
    CHECK(stream.seconds() == Approx(14.0 / 24000));
    for (s32 play = 0; play < 2; ++play) {
        std::vector<f32> samples;
        while (stream.read(samples, 1)) {
        }
        REQUIRE(samples.size() == 28);
        for (usize frame = 0; frame < 14; ++frame) {
            CHECK(samples[frame * 2] == 1.0f / 32768);
            CHECK(samples[frame * 2 + 1] == 2.0f / 32768);
        }
        CHECK_FALSE(stream.read(samples, 1));
        stream.rewind();
    }
}

TEST_CASE("native music agrees with an independent DSP decoder",
          "[audio][stream][assets][ads-reference]") {
    const char* reference = std::getenv("GDL_ADS_REFERENCE_DIR");
    if (reference == nullptr) {
        SKIP("Set GDL_ADS_REFERENCE_DIR to single-pass vgmstream PCM16 WAV exports");
    }
    for (const auto* name : {"CATH1", "CATH2A_1", "CATH2A_2", "tower"}) {
        CAPTURE(name);
        const auto wav =
            test::loadWav(std::filesystem::path(reference) / (std::string(name) + ".wav"));
        AdsStream stream;
        REQUIRE(stream.open(test::assetOrSkip(std::string("STREAMS/") + name + ".ads"),
                            AdsStream::Restoration::Disabled));
        REQUIRE(stream.info().sampleCount == wav.frames());
        REQUIRE(stream.info().sampleRate == wav.sampleRate);
        REQUIRE(stream.info().channels == wav.channels);
        std::vector<f32> samples;
        while (stream.read(samples, 997)) {
        }
        const usize trailer = stream.info().blockSize / DspAdpcmDecoder::kFrameBytes *
                              DspAdpcmDecoder::kSamplesPerFrame * wav.channels;
        REQUIRE(samples.size() == wav.samples.size() - trailer);
        usize differences = 0;
        for (usize i = 0; i < samples.size(); ++i) {
            differences += samples[i] * 32768.0f != static_cast<f32>(wav.samples[i]) ? 1U : 0U;
        }
        CHECK(differences == 0);
    }
}

TEST_CASE("Temple music produces identical playback across decode and callback boundaries",
          "[audio][stream][assets][ads-playback]") {
    const auto* name = GENERATE("CATH1", "CATH2A_1", "CATH2A_2");
    CAPTURE(name);
    const auto file = test::assetOrSkip(std::string("STREAMS/") + name + ".ads");
    AdsStream source;
    REQUIRE(source.open(file));
    std::vector<f32> pcm;
    while (source.read(pcm, 997)) {
    }
    AudioStream reference(source.desc(), 48000);
    reference.push(pcm);
    reference.finish();

    source.rewind();
    AudioStream streaming(source.desc(), 48000);
    const auto refill = [&] {
        while (!streaming.finished() && streaming.queuedSeconds() < 1.5) {
            pcm.clear();
            if (source.read(pcm, source.info().sampleRate / 2)) {
                streaming.push(pcm);
            } else {
                streaming.finish();
            }
        }
    };
    refill();
    usize differences = 0;
    usize callbacks = 0;
    while (!reference.drained()) {
        std::array<f32, 1024> expected{};
        std::array<f32, 1024> actual{};
        reference.mixInto(expected);
        streaming.mixInto(actual);
        for (usize i = 0; i < actual.size(); ++i) {
            differences += actual[i] != expected[i] ? 1U : 0U;
        }
        // Refill separately from callbacks, with enough headroom to isolate
        // chunk/compaction discontinuities from intentional queue starvation.
        if (++callbacks % 16 == 0) {
            refill();
        }
    }
    CHECK(differences == 0);
    CHECK(streaming.drained());
}

TEST_CASE("the tower's music stream decodes piece by piece and rewinds",
          "[audio][stream][assets]") {
    const auto path = test::assetOrSkip("STREAMS/tower.ads");
    AdsStream stream;
    REQUIRE_FALSE(stream.opened());
    REQUIRE(stream.open(path));
    REQUIRE(stream.opened());
    REQUIRE(stream.info().sampleRate == 44100);
    REQUIRE(stream.info().channels == 2);
    REQUIRE(stream.desc().sampleRate == 44100);
    REQUIRE(stream.desc().channels == 2);
    REQUIRE(stream.seconds() > 60.0);

    // Half a second at a time, interleaved stereo, until the stream runs out.
    std::vector<f32> out;
    REQUIRE(stream.read(out, 22050));
    REQUIRE(out.size() % 2 == 0);
    REQUIRE(out.size() > usize{2} * 1000);
    bool loud = false;
    usize frames = out.size() / 2;
    out.clear();
    while (stream.read(out, 22050)) {
        for (const f32 sample : out) {
            loud = loud || std::abs(sample) > 0.1f;
        }
        frames += out.size() / 2;
        out.clear();
    }
    REQUIRE(loud);
    REQUIRE(static_cast<f64>(frames) == Approx(stream.seconds() * stream.info().sampleRate));
    REQUIRE_FALSE(stream.read(out, 22050)); // exhausted stays exhausted

    stream.rewind();
    out.clear();
    REQUIRE(stream.read(out, 22050));
    REQUIRE(out.size() > usize{2} * 1000);
}

TEST_CASE("a missing or foreign file is not a stream", "[audio][stream]") {
    AdsStream stream;
    REQUIRE_FALSE(stream.open(test::scratchDirectory("ads-stream-none") / "none.ads"));
    REQUIRE_FALSE(stream.opened());
    std::vector<f32> out;
    REQUIRE_FALSE(stream.read(out, 100));
    REQUIRE(stream.seconds() == 0.0);
    const auto dir = test::scratchDirectory("ads-stream-bad");
    writeTextFile(dir / "bad.ads", "this is not audio at all, just some text long enough");
    REQUIRE_FALSE(stream.open(dir / "bad.ads"));
}

} // namespace
