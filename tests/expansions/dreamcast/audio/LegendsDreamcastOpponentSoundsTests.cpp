#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "game/enemies/Generators.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast opponent events reach their native mixer without GameCube or N64 banks",
          "[dreamcast-sounds][dreamcast-opponent-playback][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    const auto directory = root / "levels/levelb1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom") ||
        !std::filesystem::is_regular_file(root.parent_path() / "disc/AUDIO/AUD_DATA.ROM")) {
        SKIP("Dreamcast extraction is not installed");
    }
    test::FakeRenderDevice device;
    const auto objects = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), objects);
    map.textures =
        formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    LegendsLevel level(device, map, std::move(adapter), directory, test::dataDirectory());
    AudioMixer mixer{48000};
    AudioMixer referenceMixer{48000};
    SoundPlayer output{mixer};
    SoundPlayer reference{referenceMixer};
    LegendsDreamcastSounds sounds;
    REQUIRE(sounds.open(root));
    u32 expected = 0;
    SECTION("small troll uses native TROLL stem, not geometry name TRO") {
        REQUIRE(level.playEnemyFeedback(EnemyFeedback{.kind = 1, .tier = 1}, 10));
        expected = 0x210008;
    }
    SECTION("large close hurt uses the fixed-width native sound key") {
        REQUIRE(level.playEnemyFeedback(
            EnemyFeedback{.kind = 1, .tier = 2, .hitCount = 2, .close = true}, 8));
        expected = 0x210010;
    }
    SECTION("large close death") {
        REQUIRE(level.playEnemyFeedback(
            EnemyFeedback{.kind = 1, .tier = 2, .killed = true, .close = true}, 0));
        expected = 0x210011;
    }
    SECTION("arrow") {
        REQUIRE(level.playEnemyCue(EnemyCue{.kind = EnemyCue::Kind::Arrow}));
        expected = 0x29;
    }
    SECTION("bolt") {
        REQUIRE(level.playEnemyCue(EnemyCue{.kind = EnemyCue::Kind::Bolt}));
        expected = 0x2b;
    }
    SECTION("suicide pursuit") {
        REQUIRE(level.playEnemyCue(EnemyCue{.kind = EnemyCue::Kind::Yell}));
        expected = 0x30;
    }
    SECTION("native melee bite") {
        const auto token = sounds.namedToken("S_SCORPBITE");
        REQUIRE(token);
        REQUIRE(level.playEnemyMelee(0, 1));
        expected = *token;
    }
    SECTION("generator hit without a visual state change") {
        REQUIRE(level.playGeneratorFeedback(GeneratorEvent{.destroyed = false}));
        expected = 0x21002f;
    }
    SECTION("generator destruction") {
        REQUIRE(level.playGeneratorFeedback(GeneratorEvent{.destroyed = true}));
        expected = 0x210030;
    }
    REQUIRE(sounds.play(reference, expected) != kNoSound);
    std::array<f32, 1600> actual{};
    std::array<f32, 1600> original{};
    f64 energy = 0;
    for (usize tick = 0; tick < 60; ++tick) {
        CAPTURE(tick, expected);
        level.playSounds(output, Vec3{10000});
        output.update();
        reference.update();
        mixer.mix(actual);
        referenceMixer.mix(original);
        REQUIRE(actual == original);
        for (const auto sample : actual) {
            energy += static_cast<f64>(sample) * sample;
        }
    }
    CHECK(energy > .001);
    level.stopSounds();
    mixer.mix(actual);
    output.update();
    mixer.mix(actual);
    CHECK(output.voiceCount() == 0);
    CHECK(std::ranges::all_of(actual, [](f32 sample) { return sample == 0; }));
    level.playSounds(output, Vec3{10000});
    CHECK(output.voiceCount() == 0);
    level.stopSounds();
}
} // namespace
