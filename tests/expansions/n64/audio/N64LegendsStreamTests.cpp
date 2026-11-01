#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldData.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/audio/N64LegendsStream.h"
#include "game/audio/LevelSoundscape.h"

namespace {
using namespace gdl;

std::filesystem::path nativeAudio() {
    return test::dataDirectory().parent_path() / "assets/n64/extracted/audio";
}

std::vector<f32> render(StreamSource& stream, usize frames, usize piece) {
    std::vector<f32> pcm;
    while (pcm.size() / 2 < frames) {
        REQUIRE(stream.read(pcm, std::min(piece, frames - pcm.size() / 2)));
    }
    return pcm;
}
} // namespace

TEST_CASE("Native cathedral music is measured against actual GDL level mixer routing",
          "[legends][audio][native-music-mix-survey][assets]") {
    using namespace gdl::game;
    if constexpr (!GDL_LEGENDS_AUDIO)
        SKIP("Native MusyX audio is disabled");
    if (!std::filesystem::is_regular_file(nativeAudio() / "cathy2a.song"))
        SKIP("Native N64 music is not installed");
    const auto manifest = test::assetOrSkip("WDATA/TEMPLE.WAD");
    WorldData world;
    REQUIRE(world.load(manifest));
    const AssetLocator assets(manifest.parent_path().parent_path());
    const auto measure = [](SoundPlayer& player, AudioMixer& mixer) {
        std::array<f32, 1600> block{};
        f64 energy = 0;
        f32 peak = 0;
        bool finite = true;
        for (usize tick = 0; tick < 1800; ++tick) {
            player.update();
            mixer.mix(block);
            for (const f32 value : block) {
                finite = finite && std::isfinite(value);
                energy += static_cast<f64>(value) * value;
                peak = std::max(peak, std::abs(value));
            }
        }
        REQUIRE(finite);
        return std::array<f64, 2>{std::sqrt(energy / (1800 * block.size())), peak};
    };
    for (const f32 slider : {128.f / 255, 1.f}) {
        for (const u16 song : std::array<u16, 2>{28, 21}) {
            for (const bool normalized : {false, true}) {
                AudioMixer mixer(48000);
                SoundPlayer output(mixer);
                output.setCategoryVolume(SoundCategory::Music, slider);
                auto native = std::make_shared<N64LegendsStream>();
                REQUIRE(native->open(nativeAudio(), song));
                std::shared_ptr<StreamSource> source = native;
                if (normalized)
                    source = legendsAudioMix::prepare(source, legendsAudioMix::Source::Music);
                REQUIRE(output.playStream(source, true) != kNoSound);
                const auto levels = measure(output, mixer);
                WARN("N64 song=" << song << " normalized=" << normalized << " slider=" << slider
                                 << " rms=" << levels[0] << " peak=" << levels[1]);
            }
        }
        for (const char* stage : {"E1", "E2"}) {
            const auto* level = world.level(stage);
            REQUIRE(level);
            const auto* audio = world.audio(level->audioIndex);
            REQUIRE(audio);
            AudioMixer mixer(48000);
            SoundPlayer output(mixer);
            output.setCategoryVolume(SoundCategory::Music, slider);
            LevelSoundscape soundscape;
            soundscape.open(manifest.parent_path().parent_path(), &output, audio, 'E',
                            level->bossType >= 0);
            soundscape.startMusic(&assets, level->musicVolume);
            REQUIRE(output.isPlaying(soundscape.music()));
            const auto levels = measure(output, mixer);
            WARN("GC stage=" << stage << " stream=" << audio->stream
                             << " authoredVolume=" << level->musicVolume << " slider=" << slider
                             << " rms=" << levels[0] << " peak=" << levels[1]);
            soundscape.close();
        }
    }
}

TEST_CASE("N64 music refuses unaudited input before native sequencing",
          "[audio][stream][legends]") {
    const auto root = test::scratchDirectory("n64-music-invalid");
    N64LegendsStream stream;
    std::vector<f32> output{0.25f};
    CHECK_FALSE(stream.opened());
    CHECK(stream.desc().channels == 0);
    CHECK_FALSE(stream.read(output, 160));
    stream.rewind();
    CHECK_FALSE(stream.open(root, 39));
    CHECK_FALSE(stream.open(root, 0));
    writeTextFile(root / "gauntlet.proj", "not a native bank");
    CHECK_FALSE(stream.open(root, 0));
    CHECK(output == std::vector<f32>{0.25f});
    CHECK_FALSE(stream.opened());
}

TEST_CASE("N64 native music rewinds and renders independently of read chunk size",
          "[audio][stream][legends][assets]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    N64LegendsStream stream;
    REQUIRE(stream.open(root, 0));
    CHECK(stream.desc().sampleRate == 32000);
    CHECK(stream.desc().channels == 2);
    constexpr usize kFrames = 64000;
    const auto expected = render(stream, kFrames, 4096);
    REQUIRE(expected.size() == kFrames * 2);
    CHECK(std::any_of(expected.begin(), expected.end(), [](f32 value) { return value != 0.0f; }));
    for (const usize piece : {1U, 997U, 8191U}) {
        stream.rewind();
        REQUIRE(stream.opened());
        const auto actual = render(stream, kFrames, piece);
        CHECK(std::equal(expected.begin(), expected.end(), actual.begin(), actual.end()));
    }
    std::vector<f32> output{0.25f};
    CHECK_FALSE(stream.read(output, 0));
    REQUIRE(stream.read(output, std::numeric_limits<usize>::max()));
    CHECK(output.size() == 1 + 32768 * 2);
    CHECK(output.front() == 0.25f);
    CHECK_FALSE(stream.open(root, 0xffff));
    CHECK_FALSE(stream.opened());
    CHECK_FALSE(stream.read(output, 160));
}

TEST_CASE("N64 music sources do not share sequencer names or playback state",
          "[audio][stream][legends][assets]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    constexpr usize kFrames = 64000;
    N64LegendsStream first;
    REQUIRE(first.open(root, 0));
    const auto expectedFirst = render(first, kFrames, 997);
    N64LegendsStream second;
    REQUIRE(second.open(root, 17));
    const auto expectedSecond = render(second, kFrames, 997);
    first.rewind();
    second.rewind();
    std::vector<f32> actualFirst;
    std::vector<f32> actualSecond;
    while (actualFirst.size() / 2 < kFrames) {
        const usize count = std::min<usize>(997, kFrames - actualFirst.size() / 2);
        REQUIRE(first.read(actualFirst, count));
        REQUIRE(second.read(actualSecond, count));
    }
    CHECK(std::equal(expectedFirst.begin(), expectedFirst.end(), actualFirst.begin(),
                     actualFirst.end()));
    CHECK(std::equal(expectedSecond.begin(), expectedSecond.end(), actualSecond.begin(),
                     actualSecond.end()));
    CHECK_FALSE(first.open(root, 100));
    second.rewind();
    const auto surviving = render(second, kFrames, 4096);
    CHECK(std::equal(surviving.begin(), surviving.end(), expectedSecond.begin(),
                     expectedSecond.end()));
}

TEST_CASE("N64 music bank and song fingerprints reject modified assets",
          "[audio][stream][legends][assets]") {
    const auto source = nativeAudio();
    if (!std::filesystem::exists(source)) {
        SKIP("Native N64 music extraction is not installed");
    }
    const auto destination = test::scratchDirectory("n64-music-modified");
    constexpr std::array<std::string_view, 5> kNames{
        "gauntlet.proj", "gauntlet.pool", "gauntlet.sdir", "gauntlet.samp", "mount1.song"};
    for (const auto name : kNames) {
        writeFile(destination / name, readFile(source / name));
    }
    for (const auto name : kNames) {
        INFO(name);
        auto bytes = readFile(source / name);
        REQUIRE_FALSE(bytes.empty());
        bytes[bytes.size() / 2] ^= 1;
        writeFile(destination / name, bytes);
        N64LegendsStream stream;
        CHECK_FALSE(stream.open(destination, 0));
        CHECK_FALSE(stream.opened());
        writeFile(destination / name, readFile(source / name));
    }
}

TEST_CASE("Native N64 music follows SoundPlayer music volume and lifetime",
          "[audio][stream][legends][assets]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    auto source = std::make_shared<N64LegendsStream>();
    REQUIRE(source->open(root, 0));
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const auto handle =
        player.playStream(legendsAudioMix::prepare(source, legendsAudioMix::Source::Music), true,
                          1.0f, SoundCategory::Music);
    REQUIRE(handle != kNoSound);
    std::array<f32, 1600> output{};
    f64 energy = 0;
    for (usize frame = 0; frame < 120; ++frame) {
        mixer.mix(output);
        for (const f32 sample : output) {
            energy += static_cast<f64>(sample) * sample;
        }
        player.update();
    }
    CHECK(energy > 0.001);
    CHECK(player.isPlaying(handle));
    player.setCategoryVolume(SoundCategory::Music, 0.0f);
    // The mixer intentionally slews gains; allow one frame for the ramp.
    mixer.mix(output);
    player.update();
    mixer.mix(output);
    CHECK(std::all_of(output.begin(), output.end(), [](f32 sample) { return sample == 0.0f; }));
    player.setCategoryVolume(SoundCategory::Music, 1.0f);
    player.update();
    mixer.mix(output);
    CHECK(std::any_of(output.begin(), output.end(), [](f32 sample) { return sample != 0.0f; }));
    player.stop(handle);
    player.update();
    CHECK_FALSE(player.isPlaying(handle));
    // Stops use the same short anti-click gain ramp as other SoundPlayer voices.
    mixer.mix(output);
    mixer.mix(output);
    CHECK(std::all_of(output.begin(), output.end(), [](f32 sample) { return sample == 0.0f; }));
}

TEST_CASE("Every native N64 song sequences finite non-silent stereo audio",
          "[audio][stream][legends][assets]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    for (u16 id = 0; id < 39; ++id) {
        INFO(id);
        N64LegendsStream stream;
        REQUIRE(stream.open(root, id));
        const auto pcm = render(stream, usize{32000} * 4, 4096);
        CHECK(
            std::all_of(pcm.begin(), pcm.end(), [](f32 sample) { return std::isfinite(sample); }));
        CHECK(std::any_of(pcm.begin(), pcm.end(),
                          [](f32 sample) { return std::abs(sample) > 0.001f; }));
    }
}

TEST_CASE("N64 soundtrack uses the shared linear calibration without suppressing instruments",
          "[audio][stream][legends][assets][music-level]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    // Every track retains its entire authored envelope. Only one measured
    // playback gain changes; no block is individually normalized.
    for (const u16 song : std::array<u16, 5>{0, 17, 19, 21, 28}) {
        CAPTURE(song);
        auto source = std::make_shared<N64LegendsStream>();
        REQUIRE(source->open(root, song));
        const auto raw = render(*source, usize{32000} * 30, 4096);
        source->rewind();
        const auto calibration =
            legendsAudioMix::calibrate(*source, legendsAudioMix::Source::Music);
        REQUIRE(calibration);
        CHECK(calibration->frames == 32000 * 30);
        CHECK_FALSE(calibration->complete);
        const auto mixed = legendsAudioMix::prepare(source, legendsAudioMix::Source::Music);
        REQUIRE(mixed);
        const auto pcm = render(*mixed, usize{32000} * 30, 997);
        REQUIRE(pcm.size() == raw.size());
        f64 energy = 0;
        bool exactGain = true;
        for (usize i = 0; i < pcm.size(); ++i) {
            exactGain = exactGain && pcm[i] == raw[i] * calibration->gain;
            energy += static_cast<f64>(pcm[i]) * pcm[i];
        }
        CHECK(exactGain);
        const f64 rms = std::sqrt(energy / static_cast<f64>(pcm.size()));
        CAPTURE(rms);
        CHECK(rms > .10);
        CHECK(rms <= .30001);
        CHECK(calibration->peak * calibration->gain <= 1.80001f);
    }
}

TEST_CASE("N64 music preserves loud transients until the slider and final mixer limiter",
          "[audio][stream][legends][assets][music-level]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    constexpr u16 kSong = 27;
    constexpr usize kSeconds = 30;
    auto original = std::make_shared<N64LegendsStream>();
    REQUIRE(original->open(root, kSong));
    // Explicit unity calibration proves that the common wrapper never clips
    // headroom. Automatic RMS calibration is covered by the preceding test.
    const legendsAudioMix::Calibration unity{.kind = legendsAudioMix::Source::Music, .gain = 1};
    const auto calibrated =
        legendsAudioMix::prepare(original, legendsAudioMix::Source::Music, unity);
    REQUIRE(calibrated);
    const auto pcm = render(*calibrated, usize{32000} * kSeconds, 4096);
    REQUIRE(std::ranges::any_of(pcm, [](f32 sample) { return std::abs(sample) > 1.0f; }));

    const auto mixed = [&](u32 rate, f32 volume) {
        auto source = std::make_shared<N64LegendsStream>();
        REQUIRE(source->open(root, kSong));
        AudioMixer mixer(rate);
        SoundPlayer player(mixer);
        player.setCategoryVolume(SoundCategory::Music, volume);
        REQUIRE(player.playStream(
                    legendsAudioMix::prepare(source, legendsAudioMix::Source::Music, unity),
                    true) != kNoSound);
        std::vector<f32> output(kSeconds * rate * 2);
        for (usize at = 0; at < output.size(); at += 1280) {
            const auto count = std::min<usize>(1280, output.size() - at);
            player.update();
            mixer.mix(std::span{output}.subspan(at, count));
        }
        return output;
    };
    const auto quiet = mixed(32000, .125f);
    REQUIRE(quiet.size() == pcm.size());
    f32 maximumError = 0;
    for (usize i = 0; i < pcm.size(); ++i) {
        maximumError = std::max(maximumError, std::abs(quiet[i] - pcm[i] * .125f));
    }
    // No source clipping before the user's slider: even peaks above 1 retain
    // their shape when brought below full scale by the category volume.
    CHECK(maximumError < .00001f);
    for (const u32 rate : {32000U, 48000U}) {
        const auto loud = mixed(rate, 1.0f);
        CHECK(std::ranges::all_of(loud, [](f32 sample) {
            return std::isfinite(sample) && std::abs(sample) <= AudioMixer::kCeiling;
        }));
    }
}

TEST_CASE("Native N64 songs remain bounded through long playback and looping",
          "[audio][stream][legends][assets][.long]") {
    const auto root = nativeAudio();
    if (!std::filesystem::exists(root)) {
        SKIP("Native N64 music extraction is not installed");
    }
    for (u16 id = 0; id < 39; ++id) {
        INFO(id);
        N64LegendsStream stream;
        REQUIRE(stream.open(root, id));
        f64 tailEnergy = 0;
        for (usize second = 0; second < 600; ++second) {
            const auto pcm = render(stream, 32000, 4096);
            if (second >= 570) {
                for (const auto sample : pcm) {
                    tailEnergy += static_cast<f64>(sample) * sample;
                }
            }
        }
        // Title and movie cues have authored end markers. Do not turn those
        // one-shots into loops; these ordinary level tracks have native loops.
        if (id == 0 || id == 17 || id == 34) {
            CHECK(tailEnergy > 0.001);
        }
    }
}
