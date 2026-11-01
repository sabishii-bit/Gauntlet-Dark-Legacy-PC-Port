#include <algorithm>
#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/audio/LegendsSounds.h"
#include "expansions/shared/scenes/LegendsLevel.h"

TEST_CASE("Imported boss cues reach native effects exactly once and retain scene lifetime",
          "[legends][audio][native-boss-audio][assets]") {
    using namespace gdl;
    using namespace gdl::game;
    if constexpr (!GDL_LEGENDS_AUDIO) {
        SKIP("Native MusyX audio is disabled");
    }
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/Boss.bin")) {
        SKIP("Native N64 boss extraction is not installed");
    }
    const auto characters = test::assetOrSkip("PLAYERS").parent_path();
    constexpr std::array cases{
        std::pair{"mount6", "S_DRGDEATH"},   std::pair{"ice5", "S_YETIDEATH"},
        std::pair{"town5", "S_PLGDETH"},     std::pair{"castle5", "S_CHIMDEATH1"},
        std::pair{"boss2", "S_SKORN1DEATH"}, std::pair{"hell2", "S_SKORN2DEATH"}};
    for (const auto& [mapName, cueName] : cases) {
        CAPTURE(mapName, cueName);
        const auto directory = root / "levels" / mapName;
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                                       readFile(directory / "objects.wad"),
                                                       readFile(directory / "ipr.rom"));
        test::FakeRenderDevice device;
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        LegendsLevel level(device, map, std::move(adapter), directory, characters);
        REQUIRE(level.importedBoss());
        AudioMixer mixer(48000);
        AudioMixer referenceMixer(48000);
        SoundPlayer output(mixer);
        SoundPlayer reference(referenceMixer);
        LegendsSounds original;
        REQUIRE(original.openN64(root));
        const auto token = original.namedToken(cueName);
        REQUIRE(token);
        REQUIRE(original.play(reference, *token) != kNoSound);

        // The first cue can precede playSounds() establishing its borrowed
        // mixer. Native boss sounds remain global even for a far camera eye.
        REQUIRE(level.playOpponentSound(cueName, Vec3{0}));
        level.playSounds(output, Vec3{10000});
        REQUIRE(output.voiceCount() == 1);
        level.playSounds(output, Vec3{10000});
        CHECK(output.voiceCount() == 1);
        std::array<f32, 1600> actual{};
        std::array<f32, 1600> expected{};
        f64 energy = 0;
        for (usize tick = 0; tick < 60; ++tick) {
            CAPTURE(tick);
            output.update();
            reference.update();
            mixer.mix(actual);
            referenceMixer.mix(expected);
            REQUIRE(actual == expected);
            for (const f32 value : actual) {
                energy += static_cast<f64>(value) * value;
            }
        }
        CHECK(energy > .001);
        output.setCategoryPaused(SoundCategory::Effects, true);
        level.pauseSounds();
        mixer.mix(actual);
        output.update();
        mixer.mix(actual);
        CHECK(std::ranges::all_of(actual, [](f32 value) { return value == 0; }));
        output.setCategoryPaused(SoundCategory::Effects, false);

        // Unsupported counterpart-only names are deliberately handled, not
        // emitted as unrelated GameCube effects. Teardown discards pending cues.
        REQUIRE(level.playOpponentSound("NOT_A_NATIVE_BOSS_CUE", Vec3{0}));
        REQUIRE(level.playOpponentSound(cueName, Vec3{0}));
        level.stopSounds();
        mixer.mix(actual);
        mixer.mix(actual);
        output.update();
        CHECK(output.voiceCount() == 0);
        level.playSounds(output, Vec3{10000});
        CHECK(output.voiceCount() == 0);
        level.stopSounds();
    }
}
