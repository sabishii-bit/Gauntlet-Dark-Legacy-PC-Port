#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/audio/LegendsSpeech.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    return test::dataDirectory().parent_path() / "assets/n64/extracted";
}

bool installed() {
    return std::filesystem::is_regular_file(nativeRoot() / "_program/game.bin") &&
           std::filesystem::is_regular_file(nativeRoot() / "audio/mort.mdir");
}

std::shared_ptr<AudioStream> referenceSpeech(AudioMixer& mixer, const MortBank& bank,
                                             std::span<const u16> clips, f32 volume = 1,
                                             std::optional<f32> gain = {}) {
    auto source = bank.createRestored(clips);
    REQUIRE(source);
    if (!gain) {
        const auto calibration =
            legendsAudioMix::calibrate(*source, legendsAudioMix::Source::Speech);
        REQUIRE(calibration);
        gain = calibration->gain;
    }
    std::vector<f32> pcm;
    while (source->read(pcm, 4096)) {
    }
    REQUIRE_FALSE(pcm.empty());
    // Independent reference: scale the decoded samples before queuing them,
    // without using the production speech stream wrapper or SoundPlayer route.
    for (f32& sample : pcm) {
        sample *= *gain;
    }
    auto stream = mixer.createStream(source->desc());
    stream->setVolume(volume);
    stream->push(pcm);
    stream->finish();
    return stream;
}
} // namespace

TEST_CASE("N64 speech is optional and rejects unaudited routing metadata",
          "[legends][audio][native-speech]") {
    LegendsSpeech speech;
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    CHECK_FALSE(speech.opened());
    CHECK_FALSE(speech.resolve(0x1000));
    CHECK_FALSE(speech.entryCue("mount1"));
    CHECK_FALSE(speech.obeliskCue(1));
    CHECK_FALSE(speech.deathAdvice());
    CHECK_FALSE(speech.enqueue(output, 0x4032));
    speech.update(output, 1);
    speech.stop(output);
    CHECK(output.voiceCount() == 0);
    const auto directory = test::scratchDirectory("legends-speech-invalid");
    CHECK_FALSE(speech.openN64(directory / "absent"));
    std::filesystem::create_directories(directory / "_program");
    writeFile(directory / "_program/game.bin", std::vector<u8>(0x80000));
    CHECK_FALSE(speech.openN64(directory));
    CHECK_FALSE(speech.opened());
}

TEST_CASE("Native Death advice resolves its authored low-level caption and concrete phrase",
          "[legends][native-death-advice][native-speech][assets]") {
    if (!installed())
        SKIP("Native N64 speech extraction is not installed");
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    const auto advice = speech.deathAdvice();
    REQUIRE(advice);
    CHECK(advice->maximumLevel == 10);
    CHECK(advice->text == "Use potions to Kill Death");
    CHECK(advice->speech == 0x4028);
    const auto cue = speech.resolve(advice->speech);
    REQUIRE(cue);
    CHECK(cue->clips == std::vector<u16>{181, 179});

    // Caption availability must not depend on the optional narration files.
    const auto directory = test::scratchDirectory("legends-death-advice-no-audio");
    std::filesystem::create_directories(directory / "_program");
    std::filesystem::create_directories(directory / "english");
    writeFile(directory / "_program/game.bin", readFile(nativeRoot() / "_program/game.bin"));
    writeFile(directory / "strlib.wad", readFile(nativeRoot() / "strlib.wad"));
    writeFile(directory / "english/advisor.wad", readFile(nativeRoot() / "english/advisor.wad"));
    CHECK_FALSE(speech.openN64(directory));
    REQUIRE(speech.deathAdvice());
    CHECK(speech.deathAdvice()->text == advice->text);
    CHECK_FALSE(speech.resolve(advice->speech));
}

TEST_CASE("Native N64 entry narration follows every authored level table route",
          "[legends][audio][native-speech][assets]") {
    if (!installed()) {
        SKIP("Native N64 speech extraction is not installed");
    }
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    constexpr std::array routes{
        std::pair{"castle1", 100}, std::pair{"castle6", 101}, std::pair{"castle3", 102},
        std::pair{"castle4", 103}, std::pair{"castle5", 104}, std::pair{"mount1", 105},
        std::pair{"mount2", 106},  std::pair{"mount3", 107},  std::pair{"mount4", 108},
        std::pair{"mount5", 109},  std::pair{"mount6", 110},  std::pair{"ice1", 230},
        std::pair{"ice2", 231},    std::pair{"ice3", 232},    std::pair{"ice4", 233},
        std::pair{"ice5", 234},    std::pair{"secret1", 114}, std::pair{"secret2", 115},
        std::pair{"secret3", 116}, std::pair{"secret4", 117}, std::pair{"secret5", 239},
        std::pair{"secret6", 235}, std::pair{"town1", 225},   std::pair{"town2", 226},
        std::pair{"town3", 227},   std::pair{"town4", 228},   std::pair{"town5", 229},
        std::pair{"battle1", 236}, std::pair{"battle2", 237}, std::pair{"battle3", 238},
        std::pair{"hell1", 112},   std::pair{"hell2", 113},   std::pair{"boss1", 62},
        std::pair{"boss2", 111},
    };
    for (const auto& [name, clip] : routes) {
        CAPTURE(name);
        const auto token = speech.entryCue(name);
        REQUIRE(token);
        const auto cue = speech.resolve(*token);
        REQUIRE(cue);
        CHECK(cue->clips == std::vector<u16>{98, static_cast<u16>(clip)});
        CHECK(cue->lifetimeSeconds == 1);
        CHECK_FALSE(cue->clearQueue);
    }
    CHECK(speech.entryCue("MoUnT1") == 0x4032);
    CHECK_FALSE(speech.entryCue("tower2")); // Authored group4052 has zero clips.
    CHECK_FALSE(speech.entryCue("ice6"));   // Native level table explicitly has no entry cue.
    CHECK_FALSE(speech.entryCue("_test1"));
    CHECK_FALSE(speech.entryCue("castle2"));
    CHECK_FALSE(speech.entryCue("../mount1"));
    CHECK_FALSE(speech.entryCue(""));
}

TEST_CASE("Native N64 obelisks resolve concrete advice without effect or player substitutions",
          "[legends][audio][native-speech][assets]") {
    if (!installed()) {
        SKIP("Native N64 speech extraction is not installed");
    }
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    for (u32 identity = 1; identity <= 7; ++identity) {
        CAPTURE(identity);
        const u32 expected = identity <= 3 ? 0x402b : identity <= 5 ? 0x402e : 0x4030;
        REQUIRE(speech.obeliskCue(identity) == expected);
        const auto cue = speech.resolve(expected);
        REQUIRE(cue);
        CHECK(cue->clips == (identity <= 3   ? std::vector<u16>{48, 47, 46}
                             : identity <= 5 ? std::vector<u16>{39, 38}
                                             : std::vector<u16>{48, 35}));
    }
    CHECK_FALSE(speech.obeliskCue(0));
    CHECK_FALSE(speech.obeliskCue(8));
    CHECK_FALSE(speech.obeliskCue(std::numeric_limits<u32>::max()));
    const auto alias = speech.resolve(0x10068);
    REQUIRE(alias);
    CHECK(alias->clips == std::vector<u16>{48, 47, 46});
    CHECK(alias->clearQueue);
    const auto single = speech.resolve(0x10011);
    REQUIRE(single);
    CHECK(single->clips == std::vector<u16>{267});
    CHECK(single->lifetimeSeconds == .25f);
    CHECK_FALSE(speech.resolve(0x78)); // Obelisk activation effect remains on the MusyX path.
    CHECK_FALSE(speech.resolve(0x2000));
    CHECK_FALSE(speech.resolve(0x4000)); // Native contextual color/class/rank placeholders.
    CHECK_FALSE(speech.resolve(0x4007));
    CHECK_FALSE(speech.resolve(0x406e));
    CHECK_FALSE(speech.resolve(0x1007c));
    CHECK_FALSE(speech.resolve(0x11000));
    CHECK_FALSE(speech.resolve(0x1117)); // Clip279 does not exist.
    CHECK_FALSE(speech.resolve(0xffff));
    CHECK_FALSE(speech.resolve(0xffffffff));
}

TEST_CASE("N64 narration plays restored finite PCM with shared effects pause and volume",
          "[legends][audio][native-speech][assets]") {
    if (!installed()) {
        SKIP("Native N64 speech extraction is not installed");
    }
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    MortBank bank;
    REQUIRE(bank.open(nativeRoot() / "audio/mort.mdir"));
    AudioMixer mixer(48000);
    AudioMixer referenceMixer(48000);
    SoundPlayer output(mixer);
    REQUIRE(speech.enqueue(output, 0x4032));
    const SoundHandle original = speech.handle();
    REQUIRE(original != kNoSound);
    const std::array<u16, 2> clips{98, 105};
    const auto reference = referenceSpeech(referenceMixer, bank, clips);
    std::array<f32, 1600> samples{};
    std::array<f32, 1600> expected{};
    f64 energy = 0;
    for (usize i = 0; i < 8; ++i) {
        output.update();
        mixer.mix(samples);
        referenceMixer.mix(expected);
        REQUIRE(samples == expected);
        for (f32 sample : samples) {
            energy += static_cast<f64>(sample) * sample;
        }
    }
    output.setCategoryPaused(SoundCategory::Effects, true);
    reference->setPaused(true);
    for (usize i = 0; i < 90; ++i) {
        speech.update(output, 1.0f / 60, true);
        output.update();
        mixer.mix(samples);
        // The mixer's peak limiter still releases while paused. Advance both
        // mixers through the same silent interval, without advancing either voice.
        referenceMixer.mix(expected);
        REQUIRE(samples == expected);
        CHECK(std::ranges::all_of(samples, [](f32 value) { return value == 0; }));
        REQUIRE(output.isPlaying(original));
    }
    output.setCategoryPaused(SoundCategory::Effects, false);
    reference->setPaused(false);
    speech.update(output, 0, false);
    for (usize i = 0; i < 1200 && speech.playing(output); ++i) {
        output.update();
        mixer.mix(samples);
        referenceMixer.mix(expected);
        REQUIRE(samples == expected);
        speech.update(output, 1.0f / 60);
        for (f32 sample : samples) {
            REQUIRE(std::isfinite(sample));
            energy += static_cast<f64>(sample) * sample;
        }
    }
    CHECK(energy > .001);
    CHECK_FALSE(speech.playing(output));
    CHECK(speech.pendingCount() == 0);
    CHECK(speech.handle() == kNoSound);
    CHECK(reference->drained());
}

TEST_CASE("N64 speech pending lifetime clear and teardown stay bounded while paused",
          "[legends][audio][native-speech][assets]") {
    if (!installed()) {
        SKIP("Native N64 speech extraction is not installed");
    }
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    REQUIRE(speech.enqueue(output, 0x4032));
    const SoundHandle entry = speech.handle();
    REQUIRE(speech.enqueue(output, 0x402b));
    REQUIRE(speech.pendingCount() == 1);
    speech.update(output, 30, true);
    CHECK(speech.pendingCount() == 1);
    speech.update(output, .5f, false);
    CHECK(speech.pendingCount() == 1);
    speech.update(output, .51f);
    CHECK(speech.pendingCount() == 0);
    CHECK(output.isPlaying(entry));
    REQUIRE(speech.enqueue(output, 0x402e));
    REQUIRE(speech.enqueue(output, 0x10068)); // Authored clear-queue alias.
    CHECK_FALSE(output.isPlaying(entry));
    CHECK(speech.handle() != entry);
    CHECK(speech.playing(output));
    CHECK(speech.pendingCount() == 0);
    CHECK_FALSE(speech.enqueue(output, 0x4032, -1));
    CHECK_FALSE(speech.enqueue(output, 0x4032, 256));
    speech.update(output, std::numeric_limits<f32>::quiet_NaN());
    speech.update(output, -1);
    CHECK(speech.playing(output));
    const SoundHandle active = speech.handle();
    // Sources own their decoded bank lifetime; a failed optional reload cannot
    // leave a dangling playback cursor. The owner can still stop the handle.
    CHECK_FALSE(speech.openN64(nativeRoot() / "missing"));
    CHECK(output.isPlaying(active));
    speech.stop(output);
    CHECK_FALSE(output.isPlaying(active));
    CHECK(speech.handle() == kNoSound);
    CHECK(speech.pendingCount() == 0);
}

TEST_CASE("N64 speech waiting phrases retain native priority and equal-priority FIFO",
          "[legends][audio][native-speech][assets]") {
    if (!installed()) {
        SKIP("Native N64 speech extraction is not installed");
    }
    LegendsSpeech speech;
    REQUIRE(speech.openN64(nativeRoot()));
    MortBank bank;
    REQUIRE(bank.open(nativeRoot() / "audio/mort.mdir"));
    AudioMixer mixer(48000);
    AudioMixer referenceMixer(48000);
    SoundPlayer output(mixer);
    speech.update(output, 0, true);
    REQUIRE(speech.enqueue(output, 0x4032, 5));
    REQUIRE(speech.enqueue(output, 0x403d, 6));
    REQUIRE(speech.enqueue(output, 0x4033, 6));
    CHECK(speech.pendingCount() == 3);
    CHECK(speech.handle() == kNoSound);
    speech.update(output, 0, false);
    CHECK(speech.pendingCount() == 2);
    const std::array<u16, 2> clips{98, 100};
    const auto reference = referenceSpeech(referenceMixer, bank, clips);
    std::array<f32, 1600> actual{};
    std::array<f32, 1600> expected{};
    for (usize i = 0; i < 12; ++i) {
        output.update();
        mixer.mix(actual);
        referenceMixer.mix(expected);
        REQUIRE(actual == expected);
    }
    speech.stop(output);
    CHECK(speech.pendingCount() == 0);
    speech.update(output, 10);
    CHECK_FALSE(speech.playing(output));
}

TEST_CASE("N64 speech gain preserves peak shape before the effects slider and final limiter",
          "[legends][audio][native-speech][assets]") {
    if (!installed()) {
        SKIP("Native N64 speech extraction is not installed");
    }
    MortBank bank;
    REQUIRE(bank.open(nativeRoot() / "audio/mort.mdir"));
    constexpr std::array<u16, 1> clips{252};
    auto source = bank.createRestored(clips);
    REQUIRE(source);
    const auto calibration = legendsAudioMix::calibrate(*source, legendsAudioMix::Source::Speech);
    REQUIRE(calibration);
    std::vector<f32> pcm;
    while (source->read(pcm, 4096)) {
    }
    REQUIRE(std::ranges::any_of(
        pcm, [&](f32 sample) { return std::abs(sample) * calibration->gain > 1; }));

    for (const f32 volume : {0.0f, .5f, 1.0f}) {
        CAPTURE(volume);
        LegendsSpeech speech;
        REQUIRE(speech.openN64(nativeRoot()));
        AudioMixer mixer(48000);
        AudioMixer referenceMixer(48000);
        SoundPlayer output(mixer);
        output.setCategoryVolume(SoundCategory::Effects, volume);
        REQUIRE(speech.enqueue(output, 0x1000U | clips.front()));
        const auto reference = referenceSpeech(referenceMixer, bank, clips, volume);
        std::array<f32, 1600> samples{};
        std::array<f32, 1600> expected{};
        f64 energy = 0;
        usize chunks = 0;
        while (speech.playing(output) && chunks < 600) {
            output.update();
            mixer.mix(samples);
            referenceMixer.mix(expected);
            // At half volume this also detects any premature source clipping:
            // boosted peaks must retain their shape when the slider lowers them.
            REQUIRE(samples == expected);
            for (const f32 sample : samples) {
                REQUIRE(std::isfinite(sample));
                REQUIRE(std::abs(sample) <= AudioMixer::kCeiling);
                energy += static_cast<f64>(sample) * sample;
            }
            speech.update(output, 1.0f / 60);
            ++chunks;
        }
        CHECK(chunks < 600);
        CHECK_FALSE(speech.playing(output));
        CHECK(reference->drained());
        if (volume == 0) {
            CHECK(energy == 0);
        } else {
            CHECK(energy > 1);
        }
    }
}

TEST_CASE("All native obelisk phrases receive one measured speech gain before user volume",
          "[legends][audio][native-speech][speech-mix][assets]") {
    if (!installed()) {
        SKIP("Native N64 speech extraction is not installed");
    }
    MortBank bank;
    REQUIRE(bank.open(nativeRoot() / "audio/mort.mdir"));
    // These representatives cover all three authored advice groups, including
    // the quieter final phrase. Measure after actual 48 kHz resampling and the
    // shared limiter, not by multiplying a peak or native-rate RMS estimate.
    for (const u32 identity : {1U, 4U, 6U}) {
        for (const f32 volume : {128.0f / 255, 1.0f}) {
            CAPTURE(identity, volume);
            LegendsSpeech speech;
            REQUIRE(speech.openN64(nativeRoot()));
            const auto token = speech.obeliskCue(identity);
            REQUIRE(token);
            const auto cue = speech.resolve(*token);
            REQUIRE(cue);
            auto decoded = bank.createRestored(cue->clips);
            REQUIRE(decoded);
            const auto calibration =
                legendsAudioMix::calibrate(*decoded, legendsAudioMix::Source::Speech);
            REQUIRE(calibration);
            REQUIRE(calibration->complete);
            REQUIRE(calibration->activeRms > .01f);
            REQUIRE(calibration->peak > 0);
            const f32 expectedGain =
                std::min({.36f / calibration->activeRms, 1.8f / calibration->peak, 8.f});
            CHECK(calibration->gain == Catch::Approx(expectedGain));
            CHECK(calibration->activeRms * calibration->gain <= .36001f);
            CHECK(calibration->peak * calibration->gain <= 1.80001f);
            AudioMixer mixer(48000);
            AudioMixer referenceMixer(48000);
            SoundPlayer output(mixer);
            output.setCategoryVolume(SoundCategory::Effects, volume);
            REQUIRE(speech.enqueue(output, *token));
            const auto reference =
                referenceSpeech(referenceMixer, bank, cue->clips, volume, expectedGain);
            std::array<f32, 1600> actual{};
            std::array<f32, 1600> expected{};
            f64 energy = 0;
            usize chunks = 0;
            while (speech.playing(output) && chunks < 1200) {
                output.update();
                mixer.mix(actual);
                referenceMixer.mix(expected);
                REQUIRE(actual == expected);
                for (usize i = 0; i < actual.size(); ++i) {
                    REQUIRE(std::isfinite(actual[i]));
                    REQUIRE(std::abs(actual[i]) <= AudioMixer::kCeiling);
                    energy += static_cast<f64>(actual[i]) * actual[i];
                }
                speech.update(output, 1.0f / 60);
                ++chunks;
            }
            REQUIRE(chunks > 0);
            CHECK(chunks < 1200);
            CHECK(reference->drained());
            const f64 rms = std::sqrt(energy / static_cast<f64>(chunks * actual.size()));
            CAPTURE(rms);
            // These full phrases include authored pauses. Pin useful final
            // mixer output, rather than requiring another multiplier over the
            // superseded fixed-gain policy (which could drive clipping).
            CHECK(rms > (volume < 1 ? .11 : .21));
            CHECK_FALSE(speech.playing(output));
        }
    }
}

TEST_CASE("Town entry narration has measured presence through the real mixer",
          "[legends][audio][native-speech][speech-mix][assets]") {
    if (!installed())
        SKIP("Native N64 speech extraction is not installed");
    MortBank bank;
    REQUIRE(bank.open(nativeRoot() / "audio/mort.mdir"));
    for (const auto name : {"town1", "town2", "mount1"}) {
        CAPTURE(name);
        LegendsSpeech speech;
        REQUIRE(speech.openN64(nativeRoot()));
        const auto token = speech.entryCue(name);
        REQUIRE(token);
        const auto cue = speech.resolve(*token);
        REQUIRE(cue);
        auto decoded = bank.createRestored(cue->clips);
        REQUIRE(decoded);
        const auto calibration =
            legendsAudioMix::calibrate(*decoded, legendsAudioMix::Source::Speech);
        REQUIRE(calibration);
        const f32 oldGain =
            std::min({.30f / calibration->activeRms, 1.4f / calibration->peak, 8.f});
        const f32 expectedGain =
            std::min({.36f / calibration->activeRms, 1.8f / calibration->peak, 8.f});
        CHECK(calibration->gain == Catch::Approx(expectedGain));
        AudioMixer mixer(48000);
        AudioMixer previousMixer(48000);
        SoundPlayer output(mixer);
        constexpr f32 volume = 128.0f / 255;
        output.setCategoryVolume(SoundCategory::Effects, volume);
        REQUIRE(speech.enqueue(output, *token));
        const auto previous = referenceSpeech(previousMixer, bank, cue->clips, volume, oldGain);
        std::array<f32, 1600> actual{};
        std::array<f32, 1600> old{};
        f64 energy = 0;
        f64 oldEnergy = 0;
        usize chunks = 0;
        while (speech.playing(output) && chunks < 600) {
            output.update();
            mixer.mix(actual);
            previousMixer.mix(old);
            for (usize i = 0; i < actual.size(); ++i) {
                REQUIRE(std::isfinite(actual[i]));
                REQUIRE(std::abs(actual[i]) <= AudioMixer::kCeiling);
                energy += static_cast<f64>(actual[i]) * actual[i];
                oldEnergy += static_cast<f64>(old[i]) * old[i];
            }
            speech.update(output, 1.0f / 60);
            ++chunks;
        }
        REQUIRE(chunks > 0);
        REQUIRE(chunks < 600);
        CHECK(previous->drained());
        CHECK_FALSE(speech.playing(output));
        const f64 rms = std::sqrt(energy / static_cast<f64>(chunks * actual.size()));
        CAPTURE(rms, oldEnergy, energy);
        CHECK(rms > .14);
        CHECK(energy > oldEnergy * 1.43); // +20% amplitude, without source clipping.
    }
}
