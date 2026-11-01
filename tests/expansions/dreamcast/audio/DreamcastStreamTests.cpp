#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "expansions/dreamcast/audio/DreamcastStream.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "fixtures/ReferenceWav.h"

namespace {
using namespace gdl;

void put(std::vector<u8>& bytes, usize at, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes.at(at + i) = static_cast<u8>(value >> (i * 8));
    }
}

std::vector<u8> fixture(u32 blocks = 1) {
    std::vector<u8> bytes(0x800 + blocks * 0x1000, 0x77);
    std::fill_n(bytes.begin(), 0x800, 0);
    put(bytes, 0, 1);
    put(bytes, 4, 22050);
    put(bytes, 8, 4);
    put(bytes, 12, 0x800);
    put(bytes, 16, blocks);
    put(bytes, 20, 4);
    put(bytes, 24, 2);
    constexpr std::string_view kSignature = "Sega Stream";
    std::copy(kSignature.begin(), kSignature.end(), bytes.begin() + 0xd5);
    bytes[0x800] = bytes[0x801] = 0x10;
    bytes[0x1000] = bytes[0x1001] = 0x89;
    return bytes;
}
} // namespace

TEST_CASE("Dreamcast STR decodes planar AICA nibbles and excludes padding",
          "[audio][stream][legends]") {
    const auto path = test::scratchDirectory("dreamcast-stream") / "test.str";
    writeFile(path, fixture());
    DreamcastStream stream;
    REQUIRE_FALSE(stream.opened());
    REQUIRE(stream.open(path));
    CHECK(stream.frames() == 4);
    CHECK(stream.desc().sampleRate == 22050);
    CHECK(stream.desc().channels == 2);
    const std::array<s16, 8> expected{15, -47, 62, -62, 77, -109, 124, -124};
    for (int repeat = 0; repeat < 2; ++repeat) {
        std::vector<f32> pcm;
        CHECK_FALSE(stream.read(pcm, 0));
        while (stream.read(pcm, 1)) {
        }
        REQUIRE(pcm.size() == expected.size());
        for (usize i = 0; i < expected.size(); ++i) {
            CHECK(pcm[i] * 32768.0f == static_cast<f32>(expected[i]));
        }
        CHECK_FALSE(stream.read(pcm, 32768));
        stream.rewind();
    }
}

TEST_CASE("Dreamcast STR retains decoder state across interleaves and read sizes",
          "[audio][stream][legends]") {
    auto bytes = fixture(2);
    put(bytes, 20, 0x2000);
    std::fill(bytes.begin() + 0x800, bytes.end(), 0x77);
    std::fill(bytes.begin() + 0x1800, bytes.end(), 0);
    const auto path = test::scratchDirectory("dreamcast-interleave") / "test.str";
    writeFile(path, bytes);
    DreamcastStream stream;
    REQUIRE(stream.open(path));
    std::vector<f32> expected;
    REQUIRE(stream.read(expected, std::numeric_limits<usize>::max()));
    REQUIRE(expected.size() == 16384);
    CHECK(expected[8192] == 32767.0f / 32768.0f);
    for (const usize piece : {1U, 3U, 997U, 4097U}) {
        stream.rewind();
        std::vector<f32> actual;
        while (stream.read(actual, piece)) {
        }
        CHECK(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
    }
}

TEST_CASE("Dreamcast STR supports a mono stream without duplicating its channel",
          "[audio][stream][legends]") {
    auto bytes = fixture();
    bytes.resize(0x1000);
    put(bytes, 24, 1);
    put(bytes, 20, 1);
    const auto path = test::scratchDirectory("dreamcast-mono") / "test.str";
    writeFile(path, bytes);
    DreamcastStream stream;
    REQUIRE(stream.open(path));
    CHECK(stream.desc().channels == 1);
    CHECK(stream.frames() == 2);
    std::vector<f32> samples;
    REQUIRE(stream.read(samples, 100));
    CHECK(samples == std::vector<f32>{15.0f / 32768.0f, 62.0f / 32768.0f});
    CHECK_FALSE(stream.read(samples, 100));
}

TEST_CASE("Dreamcast STR rejects corrupt headers and resets a failed reopen",
          "[audio][stream][legends]") {
    auto bytes = fixture();
    SECTION("short header") {
        bytes.resize(100);
    }
    SECTION("foreign signature") {
        bytes[0xd5] = 0;
    }
    SECTION("wrong codec") {
        put(bytes, 8, 16);
    }
    SECTION("too many tracks") {
        put(bytes, 0, 2);
    }
    SECTION("zero channels") {
        put(bytes, 24, 0);
    }
    SECTION("channel budget") {
        put(bytes, 24, 0xffffffffU);
    }
    SECTION("invalid rate") {
        put(bytes, 4, 0);
    }
    SECTION("zero interleave") {
        put(bytes, 12, 0);
    }
    SECTION("interleave budget") {
        put(bytes, 12, 0xffffffffU);
    }
    SECTION("invalid block count") {
        put(bytes, 16, 2);
    }
    SECTION("sample extent") {
        put(bytes, 20, 0xffffffffU);
    }
    SECTION("empty payload") {
        put(bytes, 20, 0);
    }
    SECTION("truncated block") {
        bytes.pop_back();
    }
    const auto dir = test::scratchDirectory("dreamcast-malformed");
    writeFile(dir / "valid.str", fixture());
    writeFile(dir / "bad.str", bytes);
    DreamcastStream stream;
    REQUIRE(stream.open(dir / "valid.str"));
    CHECK_FALSE(stream.open(dir / "bad.str"));
    CHECK_FALSE(stream.opened());
    CHECK(stream.frames() == 0);
    CHECK(stream.seconds() == 0);
    std::vector<f32> pcm;
    CHECK_FALSE(stream.read(pcm, 100));
    stream.rewind();
    CHECK_FALSE(stream.open(dir / "absent.str"));
}

TEST_CASE("Dreamcast native streams agree with independent FFmpeg reference samples",
          "[audio][stream][legends][assets]") {
    const auto base = test::dataDirectory().parent_path() / "assets/dreamcast/extracted";
    const auto streams = base / "disc/AUDIO/STREAMS";
    if (!std::filesystem::exists(streams)) {
        SKIP("Dreamcast native stream extraction is not installed");
    }
    usize count = 0;
    usize compared = 0;
    for (const auto& entry : std::filesystem::directory_iterator(streams)) {
        if (entry.path().extension() != ".STR") {
            continue;
        }
        INFO(entry.path().string());
        DreamcastStream source;
        REQUIRE(source.open(entry.path()));
        REQUIRE(source.frames() > 0);
        std::vector<f32> prefix;
        while (prefix.size() < 40000 && source.read(prefix, 997)) {
        }
        CHECK(std::all_of(prefix.begin(), prefix.end(), [](f32 sample) {
            return std::isfinite(sample) && sample >= -1.0f && sample < 1.0f;
        }));
        source.rewind();
        std::vector<f32> repeated;
        while (repeated.size() < prefix.size()) {
            REQUIRE(
                source.read(repeated, (prefix.size() - repeated.size()) / source.desc().channels));
        }
        CHECK(std::equal(prefix.begin(), prefix.end(), repeated.begin(), repeated.end()));
        const auto reference = base / "audio-wav" / (entry.path().stem().string() + ".wav");
        if (std::filesystem::exists(reference)) {
            const auto wav = test::loadWav(reference);
            REQUIRE(source.frames() == wav.frames());
            REQUIRE(source.desc().sampleRate == wav.sampleRate);
            REQUIRE(source.desc().channels == wav.channels);
            REQUIRE(prefix.size() <= wav.samples.size());
            usize differences = 0;
            for (usize i = 0; i < prefix.size(); ++i) {
                differences += prefix[i] * 32768.0f != static_cast<f32>(wav.samples[i]) ? 1U : 0U;
            }
            CHECK(differences == 0);
            ++compared;
        }
        ++count;
    }
    CHECK(count == 50);
    if (std::filesystem::exists(base / "audio-wav/manifest.json")) {
        CHECK(compared == count);
    }
}

TEST_CASE("Dreamcast native streams reach their authored end and rewind in bounded chunks",
          "[audio][stream][legends][assets][fullstream]") {
    const auto streams =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/disc/AUDIO/STREAMS";
    if (!std::filesystem::exists(streams)) {
        SKIP("Dreamcast native stream extraction is not installed");
    }
    usize count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(streams)) {
        if (entry.path().extension() != ".STR") {
            continue;
        }
        INFO(entry.path().string());
        // Read only the native header here; decoded PCM stays bounded to one
        // chunk plus a small rewind reference, even for the longest songs.
        std::ifstream file(entry.path(), std::ios::binary);
        std::array<u8, 32> header{};
        file.read(std::bit_cast<char*>(header.data()), header.size());
        REQUIRE(file.good());
        const u32 channels = readU32LE(header, 24);
        REQUIRE(channels >= 1);
        REQUIRE(channels <= 2);
        const u64 expectedFrames = static_cast<u64>(readU32LE(header, 20)) * 2 / channels;
        REQUIRE(expectedFrames > 0);

        DreamcastStream source;
        REQUIRE(source.open(entry.path()));
        REQUIRE(source.frames() == expectedFrames);
        REQUIRE(source.desc().channels == channels);
        constexpr usize kPrefixFrames = 4097;
        std::vector<f32> prefix;
        REQUIRE(source.read(prefix, kPrefixFrames));
        REQUIRE(prefix.size() == std::min<u64>(kPrefixFrames, expectedFrames) * channels);
        u64 decodedFrames = prefix.size() / channels;
        bool validSamples = std::all_of(prefix.begin(), prefix.end(), [](f32 sample) {
            return std::isfinite(sample) && sample >= -1.0f && sample < 1.0f;
        });

        constexpr std::array<usize, 4> kRequests{997, 8191, 32768, 65536};
        usize request = 0;
        std::vector<f32> chunk;
        while (true) {
            chunk.clear();
            const usize wanted = kRequests[request++ % kRequests.size()];
            if (!source.read(chunk, wanted)) {
                REQUIRE(chunk.empty());
                break;
            }
            REQUIRE_FALSE(chunk.empty());
            REQUIRE(chunk.size() % channels == 0);
            REQUIRE(chunk.size() <= std::min<usize>(wanted, 32768) * channels);
            decodedFrames += chunk.size() / channels;
            REQUIRE(decodedFrames <= expectedFrames);
            validSamples =
                std::all_of(chunk.begin(), chunk.end(),
                            [](f32 sample) {
                                return std::isfinite(sample) && sample >= -1.0f && sample < 1.0f;
                            }) &&
                validSamples;
        }
        CHECK(validSamples);
        CHECK(decodedFrames == expectedFrames);
        for (const usize wanted : {1U, 997U, 65536U}) {
            CHECK_FALSE(source.read(chunk, wanted));
            CHECK(chunk.empty());
        }

        source.rewind();
        while (chunk.size() < prefix.size()) {
            const usize remaining = (prefix.size() - chunk.size()) / channels;
            REQUIRE(source.read(chunk, std::min<usize>(remaining, 997)));
        }
        CHECK(std::equal(prefix.begin(), prefix.end(), chunk.begin(), chunk.end()));
        ++count;
    }
    CHECK(count == 50);
}
