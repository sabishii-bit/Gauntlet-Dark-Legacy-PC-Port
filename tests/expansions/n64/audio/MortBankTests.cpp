#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/MusicDeclicker.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/io/Sha256.h"

#include "TestSupport.h"
#include "expansions/n64/audio/MortBank.h"

namespace {
using namespace gdl;

std::string sha256(std::span<const u8> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.hex();
}

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes[at] = static_cast<u8>(value >> 8);
    bytes[at + 1] = static_cast<u8>(value);
}

void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16));
    put16(bytes, at + 2, static_cast<u16>(value));
}

class ClipBuilder {
public:
    void active(u32 count) {
        bits(1, 0);
        bits(7, count - 1);
    }

    void frame(u32 seed) {
        constexpr std::array<u32, 8> kWidths{6, 6, 5, 5, 4, 4, 3, 3};
        for (usize i = 0; i < kWidths.size(); ++i) {
            bits(kWidths[i], (seed + static_cast<u32>(i) * 3U) & ((1U << kWidths[i]) - 1U));
        }
        for (u32 sub = 0; sub < 4; ++sub) {
            bits(7, (seed * 7 + sub * 19) & 127U);
            bits(2, (seed + sub) & 3U);
            bits(2, (seed + sub * 3) & 3U);
            bits(6, seed & 63U);
            for (u32 pulse = 0; pulse < 13; ++pulse) {
                bits(3, (seed + sub + pulse * 5) & 7U);
            }
        }
        ++m_frames;
    }

    void silence(u32 count) {
        bits(1, 1);
        bits(4, count - 1);
        m_frames += count;
    }

    std::vector<u8> finish(u16 rate = 8000) {
        m_bytes[0] = 'M';
        m_bytes[1] = 'O';
        m_bytes[2] = 'R';
        m_bytes[3] = 'T';
        put16(m_bytes, 4, static_cast<u16>(m_frames));
        put16(m_bytes, 6, rate);
        put32(m_bytes, 8, static_cast<u32>(m_bytes.size() / 4));
        return m_bytes;
    }

private:
    void bits(u32 count, u32 value) {
        for (u32 i = 0; i < count; ++i, ++m_bit) {
            if (m_bit / 32 * 4 == m_bytes.size()) {
                m_bytes.resize(m_bytes.size() + 4);
            }
            const usize at = m_bit / 32 * 4;
            const u32 word = readU32BE(m_bytes, at) | (((value >> i) & 1U) << (m_bit % 32));
            put32(m_bytes, at, word);
        }
    }
    std::vector<u8> m_bytes = std::vector<u8>(12);
    usize m_bit = 96;
    u32 m_frames = 0;
};

std::vector<u8> directory(std::span<const std::vector<u8>> clips) {
    std::vector<u8> bytes(4 + clips.size() * 4);
    put32(bytes, 0, static_cast<u32>(clips.size()));
    for (usize i = 0; i < clips.size(); ++i) {
        put32(bytes, 4 + i * 4, static_cast<u32>(bytes.size()));
        bytes.insert(bytes.end(), clips[i].begin(), clips[i].end());
    }
    return bytes;
}

std::vector<u8> directory(const std::vector<u8>& clip) {
    const std::array clips{clip};
    return directory(std::span<const std::vector<u8>>(clips));
}

std::vector<f32> render(StreamSource& source, usize chunk = 997) {
    std::vector<f32> pcm;
    for (usize calls = 0; calls < 200000; ++calls) {
        const usize before = pcm.size();
        if (!source.read(pcm, chunk)) {
            return pcm;
        }
        REQUIRE(pcm.size() > before);
        REQUIRE(pcm.size() - before <= std::min<usize>(chunk, 32768));
    }
    FAIL("MORT source did not reach its declared EOF");
    return {};
}

std::vector<u8> pcmBytes(std::span<const f32> pcm) {
    REQUIRE(std::ranges::all_of(
        pcm, [](f32 sample) { return std::isfinite(sample) && sample >= -1 && sample < 1; }));
    std::vector<u8> bytes(pcm.size() * 2);
    for (usize i = 0; i < pcm.size(); ++i) {
        const auto value = static_cast<s16>(pcm[i] * 32768);
        put16(bytes, i * 2, static_cast<u16>(value));
    }
    return bytes;
}

std::vector<u8> voicedClip() {
    ClipBuilder clip;
    clip.active(64);
    for (u32 seed = 0; seed < 64; ++seed) {
        clip.frame(seed);
    }
    return clip.finish();
}

std::vector<f32> restoreClip(std::span<const f32> raw, u32 sampleRate) {
    MusicDeclicker filter;
    std::vector<f32> impulses;
    filter.reset(sampleRate, 1);
    filter.feed(raw, impulses);
    filter.finish(impulses);
    std::vector<f32> restored;
    filter.reset(sampleRate, 1, MusicDeclicker::Pass::ShortBursts);
    filter.feed(impulses, restored);
    filter.finish(restored);
    return restored;
}
} // namespace

TEST_CASE("MORT decoding preserves authored silence and exact block duration",
          "[legends][audio][mort]") {
    ClipBuilder clip;
    clip.silence(16);
    clip.silence(1);
    MortBank bank;
    REQUIRE(bank.open(directory(clip.finish(11025))));
    CHECK(bank.size() == 1);
    CHECK(bank.contains(0));
    CHECK_FALSE(bank.contains(1));
    auto source = bank.create(0);
    REQUIRE(source);
    CHECK(source->desc().sampleRate == 11025);
    CHECK(source->desc().channels == 1);
    std::vector<f32> untouched{.5f};
    CHECK_FALSE(source->read(untouched, 0));
    CHECK(untouched == std::vector<f32>{.5f});
    const auto pcm = render(*source, 37);
    REQUIRE(pcm.size() == usize{17} * 160);
    CHECK(std::ranges::all_of(pcm, [](f32 sample) { return sample == 0; }));
    CHECK_FALSE(source->read(untouched, 100));
    source->rewind();
    CHECK(render(*source, std::numeric_limits<usize>::max()) == pcm);
}

TEST_CASE("MORT voices have independent predictors and survive bank reloads",
          "[legends][audio][mort]") {
    MortBank bank;
    REQUIRE(bank.open(directory(voicedClip())));
    auto first = bank.create(0);
    auto second = bank.create(0);
    REQUIRE(first);
    REQUIRE(second);
    const auto expected = render(*first, 159);
    REQUIRE(expected.size() == usize{64} * 160);
    // Independent native-MIPS oracle over this entirely synthetic vector. It
    // exercises every amplitude (including 63's masked negative shift), all gain
    // and grid codes, invalid pitch-lag fallback, clipping and wrapped history.
    CHECK(sha256(pcmBytes(expected)) ==
          "09a68f5549f5167b11a449301144a6989ac9d308c4401c779d23088b601fb769");
    CHECK(std::ranges::any_of(expected, [](f32 sample) { return sample != 0; }));
    first->rewind();
    std::vector<f32> prefix;
    REQUIRE(first->read(prefix, 17));
    CHECK(render(*second, 161) == expected);
    CHECK_FALSE(bank.open(std::span<const u8>{}));
    CHECK_FALSE(bank.opened());
    CHECK_FALSE(bank.create(0));
    auto suffix = render(*first, 23);
    prefix.insert(prefix.end(), suffix.begin(), suffix.end());
    CHECK(prefix == expected);
    second->rewind();
    CHECK(render(*second, 32768) == expected);
}

TEST_CASE("MORT silence retains rather than resets or advances its prediction history",
          "[legends][audio][mort]") {
    ClipBuilder contiguous;
    contiguous.active(4);
    for (u32 seed = 20; seed < 24; ++seed) {
        contiguous.frame(seed);
    }
    ClipBuilder paused;
    paused.active(2);
    paused.frame(20);
    paused.frame(21);
    paused.silence(3);
    paused.active(2);
    paused.frame(22);
    paused.frame(23);
    MortBank bank;
    const std::array clips{contiguous.finish(), paused.finish()};
    REQUIRE(bank.open(directory(clips)));
    const auto original = render(*bank.create(0));
    const auto separated = render(*bank.create(1));
    REQUIRE(original.size() == 640);
    REQUIRE(separated.size() == 1120);
    CHECK(std::equal(original.begin(), original.begin() + 320, separated.begin()));
    CHECK(
        std::ranges::all_of(std::span(separated).subspan(320, 480), [](f32 x) { return x == 0; }));
    CHECK(std::equal(original.begin() + 320, original.end(), separated.begin() + 800));
}

TEST_CASE("MORT concrete clip sequences reset state and reject invalid or mixed-rate IDs",
          "[legends][audio][mort]") {
    ClipBuilder silent;
    silent.silence(2);
    const std::array clips{voicedClip(), silent.finish(), silent.finish(16000)};
    MortBank bank;
    REQUIRE(bank.open(directory(clips)));
    const std::array<u16, 3> ids{0, 1, 0};
    auto sequence = bank.create(ids);
    REQUIRE(sequence);
    auto expected = render(*bank.create(0));
    const auto voice = expected;
    expected.insert(expected.end(), 320, 0);
    expected.insert(expected.end(), voice.begin(), voice.end());
    CHECK(render(*sequence, 317) == expected);
    sequence->rewind();
    CHECK(render(*sequence, 32768) == expected);
    CHECK_FALSE(bank.create(std::span<const u16>{}));
    CHECK_FALSE(bank.create(std::array<u16, 2>{0, 3}));
    CHECK_FALSE(bank.create(std::array<u16, 2>{0, 2}));
    CHECK_FALSE(bank.create(std::array<u16, 7>{}));
}

TEST_CASE("MORT restored factories reject absent invalid and mixed-rate clips",
          "[legends][audio][mort][mort-restoration]") {
    MortBank bank;
    CHECK_FALSE(bank.createRestored(0));
    CHECK_FALSE(bank.createRestored(std::array<u16, 2>{0, 1}));
    ClipBuilder silent;
    silent.silence(2);
    const std::array clips{voicedClip(), silent.finish(16000)};
    REQUIRE(bank.open(directory(clips)));
    CHECK_FALSE(bank.createRestored(2));
    CHECK_FALSE(bank.createRestored(std::span<const u16>{}));
    CHECK_FALSE(bank.createRestored(std::array<u16, 2>{0, 2}));
    CHECK_FALSE(bank.createRestored(std::array<u16, 2>{0, 1}));
    CHECK_FALSE(bank.createRestored(std::array<u16, 7>{}));
}

TEST_CASE("MORT restored playback preserves raw PCM and independent voice lifetimes",
          "[legends][audio][mort][mort-restoration]") {
    MortBank bank;
    REQUIRE(bank.open(directory(voicedClip())));
    const auto raw = render(*bank.create(0));
    const auto expected = restoreClip(raw, 8000);
    REQUIRE(expected.size() == raw.size());
    auto first = bank.createRestored(0);
    auto second = bank.createRestored(0);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->desc().sampleRate == 8000);
    CHECK(first->desc().channels == 1);
    std::vector<f32> untouched{.125f};
    CHECK_FALSE(first->read(untouched, 0));
    CHECK(untouched == std::vector<f32>{.125f});
    CHECK(std::ranges::equal(render(*first, 1), expected));
    CHECK_FALSE(first->read(untouched, 64));
    CHECK(untouched == std::vector<f32>{.125f});
    first->rewind();
    std::vector<f32> prefix;
    REQUIRE(first->read(prefix, 17));
    REQUIRE(prefix.size() == 17);
    CHECK(std::ranges::equal(render(*second, 32768), expected));
    // Requesting filtered voices must not replace the decoder's parity output.
    CHECK(sha256(pcmBytes(render(*bank.create(0)))) ==
          "09a68f5549f5167b11a449301144a6989ac9d308c4401c779d23088b601fb769");
    CHECK_FALSE(bank.open(std::span<const u8>{}));
    CHECK_FALSE(bank.createRestored(0));
    const auto suffix = render(*first, 159);
    prefix.insert(prefix.end(), suffix.begin(), suffix.end());
    CHECK(std::ranges::equal(prefix, expected));
    second->rewind();
    CHECK(std::ranges::equal(render(*second, 997), expected));
}

TEST_CASE("MORT restored phrases retain each clip's boundaries and authored duration",
          "[legends][audio][mort][mort-restoration]") {
    ClipBuilder silent;
    silent.silence(2);
    MortBank bank;
    const std::array clips{voicedClip(), silent.finish()};
    REQUIRE(bank.open(directory(clips)));
    const std::array<u16, 3> ids{0, 1, 0};
    std::vector<f32> expected;
    for (const u16 id : ids) {
        const auto raw = render(*bank.create(id));
        const auto individual = render(*bank.createRestored(id), 317);
        REQUIRE(individual.size() == raw.size());
        CHECK(std::ranges::equal(individual, restoreClip(raw, 8000)));
        expected.insert(expected.end(), individual.begin(), individual.end());
    }
    auto phrase = bank.createRestored(ids);
    REQUIRE(phrase);
    CHECK(std::ranges::equal(render(*phrase, 159), expected));
    phrase->rewind();
    CHECK(std::ranges::equal(render(*phrase, 32768), expected));
    CHECK(expected.size() == usize{64 + 2 + 64} * 160);
    CHECK(std::ranges::all_of(std::span(expected).subspan(usize{64} * 160, usize{2} * 160),
                              [](f32 value) { return value == 0; }));
}

TEST_CASE("MORT playback bypasses restoration below the filter's supported rate",
          "[legends][audio][mort][mort-restoration]") {
    auto lowRate = voicedClip();
    put16(lowRate, 6, 4000);
    MortBank bank;
    REQUIRE(bank.open(directory(lowRate)));
    const auto raw = render(*bank.create(0));
    auto source = bank.createRestored(0);
    REQUIRE(source);
    CHECK(source->desc().sampleRate == 4000);
    CHECK(source->desc().channels == 1);
    CHECK(std::ranges::equal(render(*source, 37), raw));
    source->rewind();
    CHECK(std::ranges::equal(render(*source, 32768), raw));
}

TEST_CASE("MORT preflight refuses malformed directories and truncated frame data",
          "[legends][audio][mort]") {
    const auto good = directory(voicedClip());
    MortBank bank;
    REQUIRE(bank.open(good));
    SECTION("every truncation is rejected before a source is created") {
        for (usize size = 0; size < good.size(); ++size) {
            INFO("truncation=" << size);
            CHECK_FALSE(bank.open(std::span(good).first(size)));
        }
    }
    SECTION("count and offsets cannot alias the directory or exceed the file") {
        for (const u32 value : std::array<u32, 4>{0, 1025, 0x7fffffff, 0xffffffff}) {
            auto bad = good;
            put32(bad, 0, value);
            CHECK_FALSE(bank.open(bad));
        }
        for (const u32 value : std::array<u32, 4>{0, 4, 9, 0xffffffff}) {
            auto bad = good;
            put32(bad, 4, value);
            CHECK_FALSE(bank.open(bad));
        }
    }
    SECTION("bad magic rate duration and compressed extent are rejected") {
        auto bad = good;
        bad[8] = 'X';
        CHECK_FALSE(bank.open(bad));
        for (const u16 value : std::array<u16, 4>{0, 1, 999, 65535}) {
            bad = good;
            put16(bad, 8 + 6, value);
            CHECK_FALSE(bank.open(bad));
        }
        bad = good;
        put16(bad, 8 + 4, 0);
        CHECK_FALSE(bank.open(bad));
        bad = good;
        put32(bad, 8 + 8, 0xffffffff);
        CHECK_FALSE(bank.open(bad));
    }
    SECTION("entries cannot overlap or expose a partially valid directory") {
        const std::array clips{voicedClip(), voicedClip()};
        auto bad = directory(clips);
        put32(bad, 8, readU32BE(bad, 4));
        CHECK_FALSE(bank.open(bad));
        bad = directory(clips);
        bad[readU32BE(bad, 8)] = 'X';
        CHECK_FALSE(bank.open(bad));
    }
    SECTION("declared run cannot extend past the final frame") {
        ClipBuilder clip;
        clip.silence(16);
        auto bad = directory(clip.finish());
        put16(bad, 8 + 4, 1);
        CHECK_FALSE(bank.open(bad));
    }
    SECTION("valid-looking extent cannot hide an incomplete active frame") {
        auto bad = good;
        bad.resize(bad.size() - 4);
        put32(bad, 8 + 8, static_cast<u32>((bad.size() - 8) / 4));
        CHECK_FALSE(bank.open(bad));
    }
    CHECK_FALSE(bank.opened());
    CHECK_FALSE(bank.create(0));
}

TEST_CASE("MORT file opening is opt-in and rejects unavailable data", "[legends][audio][mort]") {
    const auto dir = test::scratchDirectory("mort-bank");
    MortBank bank;
    CHECK_FALSE(bank.open(dir / "missing.mdir"));
    writeFile(dir / "synthetic.mdir", directory(voicedClip()));
    REQUIRE(bank.open(dir / "synthetic.mdir"));
    CHECK(render(*bank.create(0)).size() == usize{64} * 160);
}

TEST_CASE("MORT speech uses the engine effects mixer with resampling pause mute and stop",
          "[legends][audio][mort]") {
    const bool restored = GENERATE(false, true);
    CAPTURE(restored);
    MortBank bank;
    REQUIRE(bank.open(directory(voicedClip())));
    const auto create = [&] { return restored ? bank.createRestored(0) : bank.create(0); };
    AudioMixer mixer(48000);
    AudioMixer referenceMixer(48000);
    SoundPlayer player(mixer);
    SoundPlayer referencePlayer(referenceMixer);
    const auto handle = player.playStream(create(), false, .5f, SoundCategory::Effects);
    REQUIRE(handle != kNoSound);
    REQUIRE(referencePlayer.playStream(create(), false, .5f, SoundCategory::Effects) != kNoSound);
    std::array<f32, 1600> output{};
    std::array<f32, 1600> reference{};
    const auto mixBoth = [&] {
        player.update();
        referencePlayer.update();
        mixer.mix(output);
        referenceMixer.mix(reference);
    };
    f64 energy = 0;
    for (usize i = 0; i < 8; ++i) {
        mixBoth();
        REQUIRE(output == reference);
        for (usize frame = 0; frame < output.size() / 2; ++frame) {
            const f32 sample = output[frame * 2];
            CHECK(std::isfinite(sample));
            CHECK(output[frame * 2 + 1] == sample);
            energy += static_cast<f64>(sample) * sample;
        }
    }
    CHECK(energy > 0);
    CHECK(player.isPlaying(handle));

    // A failed bank reload must not invalidate the active voice or queued PCM.
    CHECK_FALSE(bank.open(std::span<const u8>{}));
    player.setCategoryPaused(SoundCategory::Effects, true);
    for (usize i = 0; i < 12; ++i) {
        player.update();
        mixer.mix(output);
        CHECK(std::ranges::all_of(output, [](f32 value) { return value == 0; }));
        CHECK(player.isPlaying(handle));
    }
    player.setCategoryPaused(SoundCategory::Effects, false);
    mixBoth();
    CHECK(output == reference); // No frames consumed while the category was paused.

    // Muting a volume slider, unlike pausing, follows the shared engine policy:
    // playback continues silently. Allow one buffer for its anti-click gain ramp.
    player.setCategoryVolume(SoundCategory::Effects, 0);
    mixBoth();
    mixBoth();
    CHECK(std::ranges::all_of(output, [](f32 value) { return value == 0; }));
    CHECK(player.isPlaying(handle));
    player.setCategoryVolume(SoundCategory::Effects, 1);
    mixBoth();
    mixBoth();
    CHECK(output == reference);
    CHECK(std::ranges::any_of(output, [](f32 value) { return value != 0; }));

    player.stop(handle);
    CHECK_FALSE(player.isPlaying(handle));
    for (usize i = 0; i < 3; ++i) {
        mixer.mix(output);
        player.update();
    }
    CHECK(std::ranges::all_of(output, [](f32 value) { return value == 0; }));
    CHECK(player.voiceCount() == 0);
    CHECK(mixer.streamCount() == 0);
}

TEST_CASE("Native MORT speech 252 receives selective playback restoration without retiming",
          "[legends][audio][mort][mort-restoration][assets]") {
    const auto path = test::dataDirectory().parent_path() / "assets/n64/extracted/audio/mort.mdir";
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("optional N64 MORT speech directory is not installed");
    }
    MortBank bank;
    REQUIRE(bank.open(path));
    REQUIRE(bank.contains(252));
    auto native = bank.create(252);
    REQUIRE(native);
    REQUIRE(native->desc().sampleRate == 8000);
    const auto raw = render(*native);
    REQUIRE(raw.size() == 38080);
    CHECK(sha256(pcmBytes(raw)) ==
          "577e114a9956fea300eb8b3b55fdda392b4701ee61441999651e910788c88ca9");
    auto source = bank.createRestored(252);
    REQUIRE(source);
    const auto restored = render(*source, 159);
    REQUIRE(restored.size() == raw.size());
    CHECK(std::ranges::equal(restored, restoreClip(raw, 8000)));
    CHECK(std::ranges::all_of(
        restored, [](f32 sample) { return std::isfinite(sample) && sample >= -1 && sample < 1; }));
    usize changed = 0;
    for (usize i = 0; i < raw.size(); ++i) {
        changed += restored[i] != raw[i] ? 1U : 0U;
    }
    CAPTURE(changed);
    // This is selective de-clicking, not removal of the native codec's clipping.
    // The independent scalar filter reference changes 70/38080 samples; allow
    // floating-point variation while retaining its sub-one-percent intervention.
    CHECK(changed > 0);
    CHECK(changed < raw.size() / 100);
    source->rewind();
    CHECK(std::ranges::equal(render(*source, 997), restored));
}

TEST_CASE("All 279 native MORT clips match the original MIPS decoder PCM",
          "[legends][audio][mort][assets]") {
    const auto path = test::dataDirectory().parent_path() / "assets/n64/extracted/audio/mort.mdir";
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("optional N64 MORT speech directory is not installed");
    }
    const auto bytes = readFile(path);
    // The oracle ran the original pure codec e0096720 and its synthesis callees
    // from game.bin SHA256 0b60632eb50c7e77957a5c28106b4f353ba69dccbe0b37f1f68d37a36fd6e585.
    // Only its E0000000 virtual-address relocation was changed for the headless
    // MIPS emulator. Hash is concatenated signed16-bit big-endian PCM in ID order.
    REQUIRE(sha256(bytes) == "0450434f6299ce007f039ca7062a54571cdf1ca86922c24825e11c10cb661765");
    MortBank bank;
    REQUIRE(bank.open(path));
    REQUIRE(bank.size() == 279);
    std::vector<u8> corpus;
    usize samples = 0;
    for (usize index = 0; index < bank.size(); ++index) {
        const auto id = static_cast<u16>(index);
        INFO("clip=" << id);
        auto source = bank.create(id);
        REQUIRE(source);
        CHECK(source->desc().sampleRate == 8000);
        CHECK(source->desc().channels == 1);
        const auto pcm = render(*source, 997);
        const usize offset = readU32BE(bytes, 4 + id * 4);
        CHECK(pcm.size() == static_cast<usize>(readU16BE(bytes, offset + 4)) * 160);
        const auto encoded = pcmBytes(pcm);
        corpus.insert(corpus.end(), encoded.begin(), encoded.end());
        samples += pcm.size();
    }
    CHECK(samples == 3933280);
    CHECK(sha256(corpus) == "cb854ce0e888662c3c4bf6a0a4eba9980f536c4a229cb193d1b98b7e6bb8ede6");
}
