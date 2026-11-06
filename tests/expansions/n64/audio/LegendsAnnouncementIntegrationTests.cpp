#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/SpecialMembers.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/players/PickupVoices.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct AnnouncementScene {
    std::filesystem::path common = test::assetOrSkip("PLAYERS").parent_path();
    std::filesystem::path native = test::dataDirectory().parent_path() / "assets/n64/extracted";
    AudioMixer mixer{48000};
    SoundPlayer output{mixer};
    test::FakeRenderDevice device;
    GameConfig config;
    StringTable strings;
    AssetLocator assets{common};
    std::shared_ptr<LegendsLevel> level;
    LevelWorld world;
    GameServices context; ///< what the scene reads while it stands
    PlayScene scene;

    explicit AnnouncementScene(s32 character) {
        const auto directory = native / "levels/ice1";
        if (!std::filesystem::is_regular_file(directory / "worlds.wad") ||
            !std::filesystem::is_regular_file(native / "audio/mort.mdir")) {
            SKIP("Native N64 Ice1 and dialogue extraction is not installed");
        }
        config.camera.compass = false;
        REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
        const auto objects = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects,
                                                 readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(objects, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        REQUIRE_FALSE(map.starts.empty());
        level = std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, common);
        CharacterSave save;
        save.character = character;
        save.color = 2;
        save.name = "SPEECH";
        save.progress().health = 2000;
        const std::array party{PartyMember{3, save}};
        ClassDataSet classes;
        REQUIRE(classes.load(common / "pdata"));
        LegendsTraversal traversal;
        traversal.open(map, 3, save, classes.stats(character));
        REQUIRE(traversal.authoredStart());
        level->initializeParty(party);
        auto layout = WorldLayout::fromDecoded(
            {}, {{.kind = LocatorKind::Start, .position = traversal.start()}}, map.minBounds,
            map.maxBounds);
        LevelRef ref;
        ref.realmId = -1;
        ref.realm = "Legends";
        ref.name = "n64:ice1";
        const bool loaded = world.loadImported(device, common, ref, std::move(layout),
                                               traversal.collision(), level);
        REQUIRE(loaded);
        context.config = &config;
        context.strings = &strings;
        context.assets = &assets;
        context.sounds = &output;
        context.unpackedRoot = common;
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
        for (s32 tick = 0;
             tick < 900 && (entering() || !scene.soundscape().narrationTiming().empty()); ++tick) {
            REQUIRE(scene.update(1.0 / 60, {}) == PlayOutcome::Running);
            output.update();
            std::array<f32, 1600> samples{};
            mixer.mix(samples);
        }
        REQUIRE_FALSE(entering());
        REQUIRE(scene.canPause(3));
        REQUIRE(scene.soundscape().narrationTiming().empty());
        REQUIRE(scene.actor(3));
        REQUIRE(scene.actor(3)->save().health() > 150);
        // Let the ordinary scene finish admission, then discard only previously
        // playing audio. The health event itself must use the bound live scene.
        output.stopAll();
        std::array<f32, 1600> samples{};
        for (s32 tick = 0; tick < 3; ++tick) {
            output.update();
            mixer.mix(samples);
        }
    }

    bool entering() const {
        return scene.spawning() || !level->opponentsReady() ||
               std::ranges::any_of(scene.participants(), [](const auto& player) {
                   return player.figure && player.figure->animator().entering();
               });
    }

    GDL_NON_COPYABLE_NON_MOVABLE(AnnouncementScene);
    ~AnnouncementScene() {
        scene.close();
        if (level) {
            level->stopSounds();
        }
    }
};
} // namespace

TEST_CASE("Native PlayScene health warnings replace complete supported phrases only",
          "[legends][audio][native-announcement-gameplay][assets]") {
    const s32 character = GENERATE(0, 6); // Warrior, unsupported GDL Sorceress.
    CAPTURE(character);
    AnnouncementScene fixture{character};
    auto& scene = fixture.scene;
    REQUIRE(scene.participants().size() == 1);
    REQUIRE(scene.participants().front().figure);
    auto& characterVoice = scene.participants().front().figure->voice();
    const auto name = characterVoice.find(PickupVoices::nameOf(character, 2));
    REQUIRE(name);
    const auto expectedName = characterVoice.sequence(*name);
    SoundSet narrator;
    REQUIRE(narrator.load(fixture.common / "audio/VOICE1"));
    const auto badly = narrator.find("S_BADLY");
    REQUIRE(badly);
    const auto expectedLine = narrator.sequence(*badly);
    std::vector<SoundPlayer::Event> events;
    const SoundPlayer::Capture capture(&fixture.output, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            events.push_back(event);
        }
    });
    const s32 health = scene.actor(3)->save().health();
    // DeathDrain goes through real PlayerHealth and its threshold callback,
    // without class armor making the requested final health ambiguous.
    scene.hurtPlayer(3, static_cast<f32>(health - 140), HurtKind::DeathDrain);
    REQUIRE(scene.actor(3)->save().health() == 140);
    REQUIRE(scene.soundscape().narrationBacklog() > 0);
    if (character == 0) {
        REQUIRE(events.size() == 1);
        CHECK(events.front().bank == "N64/SUMNER");
        // Native e004e668 selects 4056; e007c730 supplies Red + Warrior.
        CHECK(events.front().call == 0x03014056U);
        CHECK(events.front().after == kNoSound);
        CHECK(scene.soundscape().narrationTiming().size() == 1);
    } else {
        REQUIRE(events.size() == 2);
        CHECK(events[0].bank == expectedName.bank);
        CHECK(events[0].call == expectedName.call);
        CHECK(events[1].bank == expectedLine.bank);
        CHECK(events[1].call == expectedLine.call);
        CHECK(events[1].after == events[0].handle);
        CHECK(std::ranges::none_of(events,
                                   [](const auto& event) { return event.bank == "N64/SUMNER"; }));
        CHECK(scene.soundscape().narrationTiming().size() == 2);
    }
    for (const auto& event : events) {
        CHECK(event.category == SoundCategory::Effects);
    }
    // A second hit within the same band cannot replay the health announcement.
    scene.hurtPlayer(3, 1, HurtKind::DeathDrain);
    CHECK(scene.actor(3)->save().health() == 139);
    CHECK(events.size() == (character == 0 ? 1 : 2));
    f64 energy = 0;
    for (s32 tick = 0; tick < 240; ++tick) {
        fixture.output.update();
        std::array<f32, 1600> samples{};
        fixture.mixer.mix(samples);
        REQUIRE(std::ranges::all_of(samples, [](f32 sample) { return std::isfinite(sample); }));
        for (const f32 sample : samples) {
            energy += static_cast<f64>(sample) * sample;
        }
    }
    CHECK(energy > .001);
}
