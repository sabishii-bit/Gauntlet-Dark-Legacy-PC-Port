#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/audio/MortBank.h"
#include "expansions/n64/formats/LegendsN64Ambient.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct NativeEntry {
    std::filesystem::path root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    std::filesystem::path directory;
    test::FakeRenderDevice device;
    formats::LegendsMap map;
    std::shared_ptr<LegendsLevel> level;

    explicit NativeEntry(std::string_view stage = "ice1",
                         const std::filesystem::path& characters = {})
        : directory(root / "levels" / stage) {
        if (!std::filesystem::is_regular_file(directory / "worlds.wad") ||
            !std::filesystem::is_regular_file(root / "audio/mort.mdir")) {
            SKIP("Native N64 level and speech extraction is not installed");
        }
        const auto objects = readFile(directory / "objects.wad");
        const auto placements = readFile(directory / "ipr.rom");
        map =
            formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects, placements);
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(objects, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        REQUIRE(adapter->cameraConfig());
        REQUIRE_FALSE(map.starts.empty());
        level =
            std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory,
                                           characters.empty() ? root / "no-gc-data" : characters);
    }

    ~NativeEntry() {
        if (level) {
            level->stopSounds();
        }
    }

    std::array<CameraSubject, 1> subjects() const {
        const auto feet = map.starts.front().position;
        return {{{feet, feet + Vec3{0, 3, 0}, 3}}};
    }
};

bool silent(std::span<const f32> samples) {
    return std::ranges::all_of(samples, [](f32 sample) { return sample == 0; });
}
} // namespace

TEST_CASE("Native entry narration plays once when the level camera becomes ready",
          "[legends][entry-runtime][native-speech][assets]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    AudioMixer referenceMixer(48000);
    NativeEntry fixture;
    // Ice1's native IPR has no ambient emitters, keeping the waveform comparison
    // about the actual level-entry phrase rather than an arbitrary mix of effects.
    REQUIRE(formats::decodeLegendsN64Ambient(readFile(fixture.directory / "ipr.rom")).empty());
    fixture.level->playSounds(output, {});
    CHECK(output.voiceCount() == 0);
    fixture.level->resetCamera({});
    fixture.level->playSounds(output, {});
    CHECK(output.voiceCount() == 0);
    const auto subjects = fixture.subjects();
    fixture.level->resetCamera(subjects);
    REQUIRE(fixture.level->camera());
    fixture.level->playSounds(output, {});
    REQUIRE(output.voiceCount() == 1);
    MortBank bank;
    REQUIRE(bank.open(fixture.root / "audio/mort.mdir"));
    const std::array<u16, 2> clips{98, 230}; // Authored Ice1 entry sequence.
    auto source = bank.createRestored(clips);
    REQUIRE(source);
    const auto calibration = legendsAudioMix::calibrate(*source, legendsAudioMix::Source::Speech);
    REQUIRE(calibration);
    std::vector<f32> pcm;
    while (source->read(pcm, 4096)) {
    }
    REQUIRE_FALSE(pcm.empty());
    // Independent PCM reference includes the runtime speech-only mix gain;
    // neither the native decoder nor its restored output is modified.
    for (f32& sample : pcm) {
        sample *= calibration->gain;
    }
    auto reference = referenceMixer.createStream(source->desc());
    reference->push(pcm);
    reference->finish();
    std::array<f32, 1600> samples{};
    std::array<f32, 1600> expected{};
    f64 energy = 0;
    for (u32 frame = 0; frame < 8; ++frame) {
        fixture.level->playSounds(output, {});
        REQUIRE(output.voiceCount() == 1);
        output.update();
        mixer.mix(samples);
        referenceMixer.mix(expected);
        REQUIRE(bool{samples == expected});
    }
    SECTION("Pause freezes the voice cursor and camera while preserving the pending visit") {
        const auto camera = fixture.level->camera();
        fixture.level->pauseSounds();
        output.setCategoryPaused(SoundCategory::Effects, true);
        reference->setPaused(true);
        for (u32 frame = 0; frame < 90; ++frame) {
            output.update();
            mixer.mix(samples);
            referenceMixer.mix(expected);
            CHECK(silent(samples));
            CHECK(silent(expected));
            REQUIRE(output.voiceCount() == 1);
            CHECK(fixture.level->camera() == camera);
        }
        output.setCategoryPaused(SoundCategory::Effects, false);
        reference->setPaused(false);
        fixture.level->playSounds(output, {});
    }
    SECTION("Stopping and unloading the level releases the phrase without replay") {
        fixture.level->stopSounds();
        mixer.mix(samples);
        mixer.mix(samples);
        output.update();
        CHECK(silent(samples));
        CHECK(output.voiceCount() == 0);
        fixture.level->playSounds(output, {});
        CHECK(output.voiceCount() == 0);
        fixture.level->stopSounds();
        fixture.level.reset();
        mixer.mix(samples);
        CHECK(silent(samples));
        return;
    }
    SECTION("A repeated camera reset does not restart the entry speech") {
        fixture.level->followCamera(subjects, .2f);
        fixture.level->resetCamera(subjects);
        fixture.level->playSounds(output, {});
        REQUIRE(output.voiceCount() == 1);
    }
    for (u32 frame = 0; frame < 1200 && output.voiceCount() != 0; ++frame) {
        output.update();
        mixer.mix(samples);
        referenceMixer.mix(expected);
        REQUIRE(bool{samples == expected});
        fixture.level->playSounds(output, {});
        for (f32 sample : samples) {
            REQUIRE(std::isfinite(sample));
            energy += static_cast<f64>(sample) * sample;
        }
    }
    CHECK(energy > .001);
    CHECK(output.voiceCount() == 0);
    CHECK(reference->drained());
    fixture.level->resetCamera(subjects);
    fixture.level->playSounds(output, {});
    CHECK(output.voiceCount() == 0);
}

TEST_CASE("Native level camera advances during ordinary PlayScene materialisation",
          "[legends][entry-runtime][imported-play][assets]") {
    const auto characters = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
    NativeEntry fixture{"ice1", characters};
    ClassDataSet classes;
    REQUIRE(classes.load(characters / "pdata"));
    CharacterSave save;
    save.progress().health = 500;
    const std::array party{PartyMember{0, save}};
    LegendsTraversal traversal;
    traversal.open(fixture.map, 0, save, classes.stats(save.character));
    REQUIRE(traversal.authoredStart());
    fixture.level->initializeParty(party);
    LevelWorld world;
    auto layout =
        WorldLayout::fromDecoded({}, {{.kind = LocatorKind::Start, .position = traversal.start()}},
                                 fixture.map.minBounds, fixture.map.maxBounds);
    LevelRef ref;
    ref.realmId = -1;
    ref.realm = "Legends";
    ref.name = "n64:ice1";
    REQUIRE(world.loadImported(fixture.device, characters, ref, std::move(layout),
                               traversal.collision(), fixture.level));
    CHECK_FALSE(world.entranceCamera()); // No invented GDL camera locator overrides N64.
    GameConfig config;
    config.camera.compass = false;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    AssetLocator assets{characters};
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.assets = &assets;
    context.unpackedRoot = characters;
    PlayOptions options;
    options.welcome = false;
    PlayScene scene;
    REQUIRE(scene.open(fixture.device, context, world, party, options));
    REQUIRE(scene.spawning());
    CHECK_FALSE(scene.cinematicBars());
    CHECK(scene.hudVisible());
    REQUIRE(fixture.level->camera());
    const auto first = *fixture.level->camera();
    CHECK(scene.simulationCamera() == first);
    bool movingDuringSpawn = false;
    for (u32 tick = 0; tick < 12; ++tick) {
        REQUIRE(scene.spawning());
        REQUIRE(scene.update(1.0 / 60, {}) == PlayOutcome::Running);
        CHECK(scene.simulationCamera() == *fixture.level->camera());
        movingDuringSpawn |= fixture.level->camera()->position != first.position;
    }
    CHECK(movingDuringSpawn);
    SECTION("The normal camera and entrance clocks continue together") {
        for (u32 tick = 0; tick < 300; ++tick) {
            REQUIRE(scene.update(1.0 / 60, {}) == PlayOutcome::Running);
            CHECK(scene.simulationCamera() == *fixture.level->camera());
        }
        CHECK_FALSE(scene.spawning());
    }
    SECTION("The camera finishing cannot release a body still playing START") {
        // Advance only the actual native camera to its endpoint, not the GDL
        // body or materialisation clocks. The independently unfinished START
        // must still hold inputs and enemy admission, without cinematic bars
        // or hiding the ordinary multiplayer HUD.
        const std::array subjects{CameraSubject{scene.actor(0)->position(),
                                                scene.actor(0)->followPoint(),
                                                scene.actor(0)->height() * .5f}};
        for (u32 tick = 0; tick < 1800 && !fixture.level->opponentsReady(); ++tick) {
            fixture.level->followCamera(subjects, 1.0f / 60);
        }
        REQUIRE(fixture.level->opponentsReady());
        REQUIRE(scene.animator(0));
        REQUIRE(scene.animator(0)->entering());
        CHECK_FALSE(scene.cinematicBars());
        CHECK(scene.hudVisible());
        PlayScene::Inputs held;
        held[0].move = {{0, 1}, 1};
        held[0].attack = true;
        const Vec3 position = scene.actor(0)->position();
        u32 heldTicks = 0;
        bool heldAfterMaterialisation = false;
        for (; heldTicks < 600 && (scene.spawning() || scene.animator(0)->entering());
             ++heldTicks) {
            REQUIRE(fixture.level->opponentsReady());
            heldAfterMaterialisation |= !scene.spawning() && scene.animator(0)->entering();
            CHECK_FALSE(scene.cinematicBars());
            CHECK(scene.hudVisible());
            REQUIRE(scene.update(1.0 / 60, held) == PlayOutcome::Running);
            CHECK(glm::distance(scene.actor(0)->position(), position) < .0001f);
            CHECK(scene.missiles().count() == 0);
            CHECK(scene.enemies().count() == 0);
        }
        CHECK(heldTicks > 0);
        CHECK(heldAfterMaterialisation);
        REQUIRE_FALSE(scene.spawning());
        REQUIRE_FALSE(scene.animator(0)->entering());
        CHECK_FALSE(scene.cinematicBars());
        CHECK(scene.hudVisible());
    }
    scene.close();
}

TEST_CASE("Missing optional entry audio leaves an ordinary mixer voice untouched",
          "[legends][entry-runtime][isolation]") {
    const auto missing = test::scratchDirectory("legends-entry-no-assets");
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    SoundClip clip;
    clip.sampleRate = 48000;
    clip.channels = 1;
    clip.samples.assign(4096, .25f);
    SoundSequence sequence;
    sequence.steps.push_back({&clip, false, false});
    const auto ordinary = output.play(sequence);
    REQUIRE(ordinary != kNoSound);
    test::FakeRenderDevice device;
    formats::LegendsMap map;
    formats::LegendsMapObject object;
    object.mesh.vertices = {{{0, 0, 0}}, {{1, 0, 0}}, {{0, 0, 1}}};
    object.mesh.parts.push_back({0, 0, {0, 1, 2}});
    map.objects.push_back(std::move(object));
    {
        LegendsLevel level(device, map, std::make_unique<LegendsSceneAdapter>(),
                           missing / "levels/ice1", missing);
        const std::array<CameraSubject, 1> subjects{{{{0, 0, 0}, {0, 3, 0}, 3}}};
        level.resetCamera(subjects);
        CHECK_FALSE(level.camera());
        level.playSounds(output, {});
        level.pauseSounds();
        level.stopSounds();
    }
    CHECK(output.isPlaying(ordinary));
    CHECK(output.voiceCount() == 1);
    std::array<f32, 512> samples{};
    mixer.mix(samples);
    mixer.mix(samples);
    CHECK_FALSE(silent(samples));
    CHECK(std::filesystem::is_empty(missing));
}
