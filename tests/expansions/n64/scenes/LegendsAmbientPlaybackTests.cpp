#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct AmbientFixture {
    test::FakeRenderDevice device;
    std::unique_ptr<LegendsLevel> level;

    explicit AmbientFixture(std::string_view name) {
        if constexpr (!GDL_LEGENDS_AUDIO) {
            SKIP("Native MusyX audio is disabled in this build");
        }
        const auto directory =
            test::dataDirectory().parent_path() / "assets/n64/extracted/levels" / name;
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Native N64 Legends extraction is not installed");
        }
        const auto objects = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(objects, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        level = std::make_unique<LegendsLevel>(device, map, std::move(adapter), directory,
                                               test::assetOrSkip("PLAYERS").parent_path());
        // Before the camera is admitted, playSounds uses the supplied listener.
        // This isolates native ambient admission from the separate entry phrase.
        REQUIRE_FALSE(level->cameraAttention());
    }
};

f64 mixTicks(AudioMixer& mixer, SoundPlayer& output, usize ticks) {
    std::array<f32, 1600> samples{};
    f64 energy = 0;
    for (usize tick = 0; tick < ticks; ++tick) {
        output.update();
        mixer.mix(samples);
        REQUIRE(std::ranges::all_of(samples, [](f32 value) { return std::isfinite(value); }));
        for (const f32 value : samples) {
            energy += static_cast<f64>(value) * value;
        }
    }
    output.update();
    return energy;
}
} // namespace

TEST_CASE("Native level waterfall audio starts once leaves range and stops on teardown",
          "[legends][ambient-playback][native-ambient][audio][assets]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    AmbientFixture fixture{"mount1"};
    // Exact first IPR waterfall; the second is outside its own radius here.
    const Vec3 waterfall{-42.5f, 171.25f, -89.75f};
    fixture.level->playSounds(output, waterfall);
    REQUIRE(output.voiceCount() == 1);
    for (usize i = 0; i < 12; ++i) {
        fixture.level->playSounds(output, waterfall);
    }
    CHECK(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .001);
    fixture.level->playSounds(output, waterfall + Vec3{0, 1000, 0});
    mixTicks(mixer, output, 2); // Let the common mixer's stop ramp finish.
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
    fixture.level->playSounds(output, waterfall);
    REQUIRE(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .001);
    // PlayScene stops imported sounds before releasing its native level.
    fixture.level->stopSounds();
    fixture.level.reset();
    mixTicks(mixer, output, 2);
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
}

TEST_CASE("Native level ambient audio resumes at the same PCM cursor after effects pause",
          "[legends][ambient-playback][native-ambient][audio][assets]") {
    AudioMixer mixer(48000);
    AudioMixer referenceMixer(48000);
    SoundPlayer output(mixer);
    SoundPlayer reference(referenceMixer);
    AmbientFixture fixture{"mount1"};
    const Vec3 waterfall{-42.5f, 171.25f, -89.75f};
    LegendsSounds sounds;
    REQUIRE(sounds.openN64(test::dataDirectory().parent_path() / "assets/n64/extracted"));
    REQUIRE(sounds.play(reference, 7, 1, false) != kNoSound);
    fixture.level->playSounds(output, waterfall);
    REQUIRE(output.voiceCount() == 1);
    std::array<f32, 1600> samples{};
    std::array<f32, 1600> expected{};
    const auto compareTick = [&] {
        output.update();
        reference.update();
        mixer.mix(samples);
        referenceMixer.mix(expected);
        REQUIRE(samples == expected);
    };
    for (usize i = 0; i < 12; ++i) {
        compareTick();
    }
    output.setCategoryPaused(SoundCategory::Effects, true);
    fixture.level->pauseSounds();
    for (usize i = 0; i < 90; ++i) {
        output.update();
        mixer.mix(samples);
        CHECK(std::ranges::all_of(samples, [](f32 value) { return value == 0; }));
        REQUIRE(output.voiceCount() == 1);
    }
    output.setCategoryPaused(SoundCategory::Effects, false);
    fixture.level->playSounds(output, waterfall);
    for (usize i = 0; i < 120; ++i) {
        compareTick();
    }
    fixture.level->stopSounds();
    reference.stopAll();
}

TEST_CASE("Overlapping native ambient placements retain separate same-cue voices",
          "[legends][ambient-playback][native-ambient][audio][assets]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    AmbientFixture fixture{"mount3"};
    // The first two authored cue277 regions overlap here. The third does not.
    const Vec3 overlap{-75, 40, 29};
    fixture.level->playSounds(output, overlap);
    REQUIRE(output.voiceCount() == 2);
    CHECK(mixTicks(mixer, output, 120) > .001);
    fixture.level->playSounds(output, overlap);
    CHECK(output.voiceCount() == 2);
    const Vec3 first{-141.25f, 35.25f, 24.75f};
    fixture.level->playSounds(output, first);
    mixTicks(mixer, output, 2);
    REQUIRE(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .001);
    fixture.level->playSounds(output, overlap);
    REQUIRE(output.voiceCount() == 2);
    CHECK(mixTicks(mixer, output, 12) > .001);
    fixture.level->stopSounds();
    mixTicks(mixer, output, 2);
    CHECK(output.voiceCount() == 0);
}

TEST_CASE("Native finite ambient samples are not replayed while remaining inside their zone",
          "[legends][ambient-playback][native-ambient][audio][assets]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    AmbientFixture fixture{"ice3"};
    // Only cue276 is audible at its exact authored position. Its sample is not
    // looped; the native macro waits for key-off after the slow sample finishes.
    const Vec3 finite{-24, -69.5f, 112};
    fixture.level->playSounds(output, finite);
    REQUIRE(output.voiceCount() == 1);
    f64 energy = 0;
    f64 lateEnergy = 0;
    for (usize second = 0; second < 20; ++second) {
        fixture.level->playSounds(output, finite);
        const f64 part = mixTicks(mixer, output, 60);
        energy += part;
        if (second >= 19) {
            lateEnergy += part;
        }
        CHECK(output.voiceCount() <= 1);
    }
    REQUIRE(energy > .001);
    CHECK(lateEnergy < energy * .000001);
    // Even an ended/externally stopped output handle must remain admitted until
    // this emitter leaves range; testing only a live waiting macro misses this.
    output.stopAll();
    mixTicks(mixer, output, 2);
    REQUIRE(output.voiceCount() == 0);
    fixture.level->playSounds(output, finite);
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
    fixture.level->playSounds(output, finite + Vec3{0, 1000, 0});
    fixture.level->playSounds(output, finite);
    REQUIRE(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .001);
    fixture.level->stopSounds();
}
