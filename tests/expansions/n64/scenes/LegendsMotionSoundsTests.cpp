#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Ambient.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct MotionFixture {
    test::FakeRenderDevice device;
    std::unique_ptr<LegendsLevel> level;
    LegendsSceneAdapter* adapter = nullptr;
    Vec3 platePosition{0};
    Vec3 soundAnchor{0};
    u32 plateSound = 0;
    std::vector<formats::LegendsN64Ambient> ambience;

    MotionFixture() {
        const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
        const auto directory = root / "levels/mount1";
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Native N64 Legends extraction is not installed");
        }
        const auto objects = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
        const auto elevator =
            std::ranges::find(map.objects, std::string{"ELEV1"}, &formats::LegendsMapObject::name);
        REQUIRE(elevator != map.objects.end());
        // The native waterfall's radius126 encloses this platform. Relocate
        // this test's target/listener anchor, not
        // its authored plate trigger or motion timings, to isolate moving and
        // endpoint voices without disabling real ambient behavior.
        elevator->transform[3].x += 4096;
        soundAnchor = Vec3{elevator->transform[3]};
        ambience = formats::decodeLegendsN64Ambient(readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(objects, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        const auto definitions = formats::decodeLegendsN64Switches(
            readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
        const auto plate =
            std::ranges::find(definitions.switches, 2U, &formats::LegendsN64Switch::nativeIndex);
        REQUIRE(plate != definitions.switches.end());
        plateSound = plate->sound;
        REQUIRE(plateSound != 0);
        const LegendsWorldMotion motion{map, definitions.switches};
        REQUIRE(motion.platePosition(2));
        platePosition = *motion.platePosition(2);
        auto owned = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        adapter = owned.get();
        level = std::make_unique<LegendsLevel>(device, map, std::move(owned), directory,
                                               test::assetOrSkip("PLAYERS").parent_path());
    }

    Vec3 begin() {
        REQUIRE(adapter->soundLoops().empty());
        adapter->touchSwitches(std::array{LegendsPortalVisitor{0, platePosition, true, .5f, 3}});
        // A plate press is a different native cue from the target's retained
        // movement handle. Isolate the latter's lifetime in this regression.
        adapter->takeSounds();
        level->update(1.0f / 60, false);
        const auto loops = adapter->soundLoops();
        REQUIRE(loops.size() == 1);
        CHECK(loops[0].token == 0x1fe);
        CHECK(loops[0].route == LegendsSoundRoute::Global);
        CHECK(loops[0].position.x == soundAnchor.x);
        CHECK(loops[0].position.z == soundAnchor.z);
        for (const auto& ambient : ambience) {
            REQUIRE(glm::distance(loops[0].position, ambient.position) > ambient.radius + 70);
        }
        return loops[0].position;
    }
};

f64 mixTicks(AudioMixer& mixer, SoundPlayer& output, usize count) {
    std::array<f32, 1600> samples{};
    f64 energy = 0;
    for (usize tick = 0; tick < count; ++tick) {
        mixer.mix(samples);
        for (const f32 value : samples) {
            energy += static_cast<f64>(value) * value;
        }
        output.update();
    }
    return energy;
}
} // namespace

TEST_CASE("Native moving platform audio pauses resumes without invented distance attenuation",
          "[legends][motion-sounds][audio][assets]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    MotionFixture fixture;
    const Vec3 listener = fixture.begin();
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    for (usize tick = 0; tick < 12; ++tick) {
        fixture.level->playSounds(output, listener);
    }
    CHECK(output.voiceCount() == 1); // No start per simulation/render tick.
    CHECK(mixTicks(mixer, output, 12) > .000001);

    fixture.level->pauseSounds();
    mixTicks(mixer, output, 2); // Shared mixer anti-click stop ramp.
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
    CHECK(fixture.adapter->soundLoops().size() == 1);
    CHECK(fixture.adapter->takeSounds().empty()); // Pause is not an endpoint.
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .000001);

    // A shared output reset invalidates the old handle, not the native motion.
    output.stopAll();
    mixTicks(mixer, output, 2);
    REQUIRE(output.voiceCount() == 0);
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .000001);

    fixture.level->playSounds(output, listener + Vec3{1000, 0, 0});
    mixTicks(mixer, output, 2);
    CHECK(output.voiceCount() == 1);
    CHECK(mixTicks(mixer, output, 12) > .000001);
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    fixture.level->stopSounds(); // Leaving while still moving stops the retained handle.
    mixTicks(mixer, output, 2);
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
}

TEST_CASE("Native movement completion replaces the loop with one endpoint effect",
          "[legends][motion-sounds][audio][assets]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    MotionFixture fixture;
    const Vec3 listener = fixture.begin();
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    fixture.level->update(3, true); // Stop Time leaves ordinary scenery in motion.
    REQUIRE(fixture.adapter->soundLoops().empty());
    fixture.level->playSounds(output, listener);
    mixTicks(mixer, output, 2);
    REQUIRE(output.voiceCount() == 1); // End cue remains, moving handle is gone.
    fixture.level->pauseSounds();
    CHECK(output.voiceCount() == 1); // One-shot tails remain audible under pause.
    CHECK(mixTicks(mixer, output, 30) > .000001);
    fixture.level->stopSounds();
    mixTicks(mixer, output, 2);
    REQUIRE(output.voiceCount() == 0);
    fixture.level->update(1, false);
    fixture.level->playSounds(output, listener);
    CHECK(output.voiceCount() == 0);
    CHECK(mixTicks(mixer, output, 12) == 0);
    fixture.level->stopSounds();
}

TEST_CASE("Native plate activation retains its authored volume away from the listener",
          "[legends][motion-sounds][audio][assets]") {
    AudioMixer mixer(48000);
    AudioMixer referenceMixer(48000);
    SoundPlayer output(mixer);
    SoundPlayer referenceOutput(referenceMixer);
    MotionFixture fixture;
    LegendsSounds direct;
    REQUIRE(direct.openN64(test::dataDirectory().parent_path() / "assets/n64/extracted"));
    fixture.adapter->touchSwitches(
        std::array{LegendsPortalVisitor{0, fixture.platePosition, true, .5f, 3}});
    // Before the first motion tick, this is just the actual plate cue. Listen
    // outside every authored ambient zone so its PCM can be compared exactly
    // with the same native macro played without any world-distance routing.
    REQUIRE(fixture.adapter->soundLoops().empty());
    const Vec3 listener = fixture.soundAnchor + Vec3{1000, 0, 0};
    REQUIRE(glm::distance(listener, fixture.platePosition) > 70);
    for (const auto& ambient : fixture.ambience) {
        REQUIRE(glm::distance(listener, ambient.position) > ambient.radius);
    }
    fixture.level->playSounds(output, listener);
    REQUIRE(output.voiceCount() == 1);
    REQUIRE(direct.play(referenceOutput, fixture.plateSound, 1, false) !=
            kNoSound);
    std::array<f32, 1600> actual{};
    std::array<f32, 1600> expected{};
    f64 energy = 0;
    for (usize i = 0; i < 30; ++i) {
        output.update();
        referenceOutput.update();
        mixer.mix(actual);
        referenceMixer.mix(expected);
        REQUIRE(actual == expected);
        for (const f32 sample : actual) {
            energy += static_cast<f64>(sample) * sample;
        }
    }
    CHECK(energy > .000001);
    fixture.level->stopSounds();
}
