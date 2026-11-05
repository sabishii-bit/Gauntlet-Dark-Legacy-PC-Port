#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundClip.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/audio/DreamcastStream.h"
#include "expansions/n64/audio/MortBank.h"
#include "expansions/n64/audio/N64LegendsEffects.h"
#include "expansions/n64/audio/N64LegendsStream.h"
#include "game/characters/CharacterSave.h"
#include "game/config/GameConfig.h"

namespace {

using namespace gdl;
using namespace gdl::game;

// These tests require neither native assets nor the optional MusyX dependency.
// The disabled backend must expose the same harmless closed-source contract.
TEST_CASE("missing expansion audio leaves the common mixer and its settings intact",
          "[expansions][isolation][audio]") {
    const auto installation = test::scratchDirectory("expansion-audio-isolation");
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    player.setMasterVolume(0.5f);
    player.setCategoryVolume(SoundCategory::Effects, 0.25f);
    player.setCategoryVolume(SoundCategory::Music, 0.75f);

    SoundClip clip;
    clip.sampleRate = 48000;
    clip.channels = 1;
    clip.samples.assign(4096, 0.5f);
    SoundSequence sequence;
    sequence.steps.push_back({&clip, false, false});
    const auto ordinaryVoice = player.play(sequence);
    REQUIRE(ordinaryVoice != kNoSound);
    std::array<f32, 512> gainRamp{};
    mixer.mix(gainRamp); // Settle the common mixer's five-millisecond gain ramp.
    std::array<f32, 128> before{};
    mixer.mix(before);
    REQUIRE(std::ranges::any_of(before, [](f32 value) { return value > 0; }));

    {
        auto music = std::make_shared<N64LegendsStream>();
        auto disc = std::make_shared<DreamcastStream>();
        N64LegendsEffects effects;
        MortBank speech;
        CHECK_FALSE(music->open(installation / "assets/n64/extracted/audio", 0));
        CHECK_FALSE(disc->open(installation / "assets/dreamcast/music.STR"));
        CHECK_FALSE(effects.open(installation / "assets/n64/extracted"));
        CHECK_FALSE(speech.open(installation / "assets/n64/extracted/audio/mort.mdir"));
        CHECK_FALSE(speech.open(std::span<const u8>{}));
        CHECK_FALSE(music->opened());
        CHECK_FALSE(disc->opened());
        CHECK_FALSE(effects.opened());
        CHECK(effects.cueIds().empty());
        CHECK_FALSE(effects.contains(22));
        CHECK_FALSE(effects.create(22));
        CHECK_FALSE(speech.opened());
        CHECK(speech.size() == 0);
        CHECK_FALSE(speech.contains(0));
        CHECK_FALSE(speech.create(0));
        CHECK_FALSE(speech.create(std::array<u16, 2>{0, 1}));
        CHECK(player.playStream(music, true) == kNoSound);
        CHECK(player.playStream(disc, true) == kNoSound);
        CHECK(player.playStream(speech.create(0), false) == kNoSound);

        std::vector<f32> untouched{0.125f, -0.25f};
        CHECK_FALSE(music->read(untouched, 64));
        CHECK_FALSE(disc->read(untouched, 64));
        CHECK(untouched == std::vector<f32>{0.125f, -0.25f});
        music->rewind();
        disc->rewind();
    }

    player.update();
    CHECK(player.isPlaying(ordinaryVoice));
    CHECK(player.voiceCount() == 1);
    CHECK(player.masterVolume() == 0.5f);
    CHECK(player.categoryVolume(SoundCategory::Effects) == 0.25f);
    CHECK(player.categoryVolume(SoundCategory::Music) == 0.75f);
    std::array<f32, 128> after{};
    mixer.mix(after);
    CHECK(after == before);
    CHECK(std::filesystem::is_empty(installation));
}

TEST_CASE("GameCube settings and save slots work without optional expansion directories",
          "[expansions][isolation][config][save]") {
    const auto installation = test::scratchDirectory("expansion-save-isolation");
    GameConfig config;
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    config.audio.musicVolume = 0.375f;
    config.audio.movieVolume = 0.625f;
    config.game.autoSave = true;
    config.save.directory = "saves";
    const auto settings = GameConfig::userSettingsPath(installation);
    REQUIRE(settings == installation / "config/settings.json");
    config.saveFile(settings);
    const auto settingsBefore = readTextFile(settings);

    CharacterSave character;
    character.name = "LOCAL";
    character.character = 2;
    character.color = 1;
    character.gold = 4321;
    character.progress().health = 640;
    character.progress().experience = 1234;
    character.progress().crystals[1] = 17;
    character.progress().inventory.keys = 4;
    character.progress().inventory.potions = {1, 3};
    REQUIRE(character.progress().relics.addRune(4));
    REQUIRE(character.progress().relics.addShard(2));
    SaveSlots saves;
    REQUIRE(saves.open(config.saveDirectory(installation), 4));
    REQUIRE(saves.write(1, character));
    const auto characterBefore = readTextFile(saves.path(1));

    // Failed optional asset probes must not migrate or rewrite the ordinary save/config.
    N64LegendsStream music;
    N64LegendsEffects effects;
    DreamcastStream disc;
    MortBank speech;
    CHECK_FALSE(music.open(installation / "assets/n64/extracted/audio", 0));
    CHECK_FALSE(effects.open(installation / "assets/n64/extracted"));
    CHECK_FALSE(disc.open(installation / "assets/dreamcast/music.STR"));
    CHECK_FALSE(speech.open(installation / "assets/n64/extracted/audio/mort.mdir"));
    CHECK(readTextFile(settings) == settingsBefore);
    CHECK(readTextFile(saves.path(1)) == characterBefore);

    GameConfig restoredConfig;
    REQUIRE(restoredConfig.loadFile(settings));
    CHECK(restoredConfig.audio.musicVolume == config.audio.musicVolume);
    CHECK(restoredConfig.audio.movieVolume == config.audio.movieVolume);
    CHECK(restoredConfig.game.autoSave);
    SaveSlots restoredSlots;
    REQUIRE(restoredSlots.open(restoredConfig.saveDirectory(installation), 4));
    const auto restored = restoredSlots.load(1);
    REQUIRE(restored.has_value());
    CHECK(restored->toJson() == character.toJson());
    for (const auto& progress : restored->classes) {
        CHECK(progress.legendsProgress(LegendsPlatform::N64) == LegendsProgress{});
        CHECK(progress.legendsProgress(LegendsPlatform::Dreamcast) == LegendsProgress{});
    }
    CHECK_FALSE(std::filesystem::exists(installation / "assets"));
    CHECK_FALSE(std::filesystem::exists(installation / "n64"));
    CHECK_FALSE(std::filesystem::exists(installation / "dreamcast"));
}

} // namespace
