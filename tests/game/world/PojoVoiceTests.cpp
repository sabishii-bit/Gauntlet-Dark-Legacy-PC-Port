#include <array>
#include <cmath>
#include <format>
#include <numeric>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/players/NameCheats.h"
#include "game/players/PickupVoices.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/PartyHud.h"
#include "game/screens/PlayScene.h"
#include "game/screens/PlayerSelectScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("EGG911 retains Pojo's spoken identity across every class and color",
          "[pojo][pojo-voice][audio]") {
    for (s32 character = 0; character <= kSumnerClass; ++character) {
        for (s32 color = 0; color < kColorCount; ++color) {
            CAPTURE(character, color);
            CharacterSave save;
            save.name = "EGG911";
            save.selectClass(character);
            save.color = color;
            REQUIRE(applyNameCheats(save));
            CHECK(save.progress().inventory.powerupCount() == 0);
            CHECK(PickupVoices::carriesPojo(save));
            save = CharacterSave::fromJson(save.toJson());
            CHECK(PickupVoices::carriesPojo(save));
            save.name = "PLAYER";
            restoreNameForm(save);
            CHECK_FALSE(PickupVoices::carriesPojo(save));
        }
    }
}

TEST_CASE("selection greets EGG911 with the native trailing Pojo name on new and loaded saves",
          "[pojo][pojo-voice][select][audio][assets]") {
    // AudioWithName (800A01A8): a leading welcome selects S_POJO1 (1002A),
    // while a name before another sentence selects S_POJO2 (1002B), both VOICE1.
    const bool returning = GENERATE(false, true);
    const bool pojo = GENERATE(false, true);
    CAPTURE(returning, pojo);
    const auto root = test::assetOrSkip("SELECT/textures.ngc").parent_path().parent_path();
    SoundSet select;
    SoundSet narrator;
    SoundSet common;
    REQUIRE(select.load(root / "AUDIO/SELECT"));
    REQUIRE(narrator.load(root / "AUDIO/VOICE1"));
    REQUIRE(common.load(root / "AUDIO/COMMON"));
    REQUIRE(narrator.find("S_POJO1"));
    CHECK(narrator.entry(*narrator.find("S_POJO1")).id == 0x1002A);
    REQUIRE(narrator.find("S_POJO2"));
    CHECK(narrator.entry(*narrator.find("S_POJO2")).id == 0x1002B);

    GameConfig config;
    config.save.directory = test::scratchDirectory("pojo-voice-select").string();
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    CharacterSave save;
    save.name = pojo ? "EGG911" : "PLAYER";
    save.selectClass(4);
    save.color = 3;
    if (returning) {
        save.progress().experience = levelExperience(2);
    }
    SaveSlots slots;
    REQUIRE(slots.open(config.saveDirectory(), config.save.slots));
    REQUIRE(slots.write(0, save));
    test::FakeRenderDevice device;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = root;
    context.sounds = &sounds;
    PlayerSelectScene scene;
    REQUIRE(scene.open(device, context, 2));
    PlayerSelectScene::Inputs input{};
    if (returning) {
        input[2].select = true;
        scene.step(1, input); // Load is initially focused when a save exists.
        scene.step(1, input);
        scene.step(SelectLane::kOperationStepTicks * 3, {});
        scene.step(SelectLane::kNoticeTicks, {});
    } else {
        input[2].up = true;
        scene.step(1, input);
        input[2] = {};
        input[2].select = true;
        scene.step(1, input); // New
        input[2] = {};
        input[2].typed = save.name;
        scene.step(1, input);
        input[2] = {};
        input[2].select = true;
        scene.step(1, input);
        scene.step(NameEntry::kFlashTicks + 1, {});
    }
    REQUIRE(scene.lane(2).state() == SelectLane::State::ClassPick);
    REQUIRE(scene.lane(2).save().name == save.name);
    sounds.stopAll();
    std::array<f32, 96000> flush{};
    mixer.mix(flush);
    sounds.update();
    scene.step(1, input);
    REQUIRE(scene.lane(2).state() ==
            (returning ? SelectLane::State::SaveMenu : SelectLane::State::LockedIn));
    REQUIRE(scene.speaking());

    // Compare the actual mixed greeting, not just the presence of a queued handle.
    AudioMixer expectedMixer(48000);
    SoundPlayer expected(expectedMixer);
    REQUIRE(common.find("S_OPTMENUSEL"));
    expected.play(common.sequence(*common.find("S_OPTMENUSEL")));
    const auto welcome = select.find(returning ? "S_WELCOMEBACK" : "S_WELCOME");
    REQUIRE(welcome);
    const auto first = expected.play(select.sequence(*welcome));
    const auto& chosen = scene.lane(2).save();
    const auto name =
        pojo ? std::string{"S_POJO1"}
             : std::format("S_{}{}1S", colorCode(chosen.color), classCode(chosen.character));
    SoundSet& bank = pojo ? narrator : select;
    REQUIRE(bank.find(name));
    const auto last = expected.playAfter(first, bank.sequence(*bank.find(name)));
    f64 error = 0;
    f64 energy = 0;
    for (s32 chunk = 0; chunk < 200 && expected.isPlaying(last); ++chunk) {
        std::array<f32, 9600> actual{};
        std::array<f32, 9600> reference{};
        mixer.mix(actual);
        expectedMixer.mix(reference);
        for (usize i = 0; i < actual.size(); ++i) {
            error += std::abs(actual[i] - reference[i]);
            energy += std::abs(reference[i]);
        }
        sounds.update();
        expected.update();
    }
    REQUIRE_FALSE(expected.isPlaying(last));
    CHECK(energy > 1);
    CHECK(error < energy * 0.0001);
    CHECK_FALSE(scene.speaking());
}

TEST_CASE("Tower promotion speaks Pojo instead of the underlying class name",
          "[pojo][pojo-voice][promotion][audio][assets]") {
    const bool pojo = GENERATE(false, true);
    const auto root = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    CharacterSave save;
    save.name = pojo ? "EGG911" : "PLAYER";
    save.progress().experience = levelExperience(10);
    save.progress().promotedLevel = 1;
    applyNameCheats(save);
    const std::array party{PartyMember{0, save}};
    const GameConfig config;
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameContext context;
    context.config = &config;
    context.unpackedRoot = root;
    context.sounds = &sounds;
    PlayScene scene;
    // Keep unrelated ambient emitters outside listening range for the audio comparison.
    PlayOptions options;
    options.position = Vec3{10000, 0, 10000};
    options.welcome = false;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.promotion().active());
    sounds.stopAll();
    std::array<f32, 96000> flush{};
    mixer.mix(flush);
    sounds.update();
    for (s32 tick = 0; tick < 600 && sounds.voiceCount() == 0; ++tick) {
        scene.update(1.0 / 60, {});
    }
    REQUIRE(sounds.voiceCount() > 0);
    SoundSet bank;
    REQUIRE(bank.load(root / (pojo ? "AUDIO/VOICE1" : "AUDIO/WAR")));
    const auto name = bank.find(pojo ? "S_POJO2" : "S_YELWAR2");
    REQUIRE(name);
    AudioMixer expectedMixer(48000);
    SoundPlayer expected(expectedMixer);
    expected.play(bank.sequence(*name));
    std::array<f32, 9600> actual{};
    std::array<f32, 9600> reference{};
    mixer.mix(actual);
    expectedMixer.mix(reference);
    CHECK(actual == reference);
    CHECK(std::ranges::any_of(reference, [](f32 sample) { return sample != 0; }));
}

TEST_CASE("Pojo gameplay announcements use the transformed identity without an inventory slot",
          "[pojo][pojo-voice][party-hud][audio]") {
    const bool pojo = GENERATE(false, true);
    const s32 lesson = GENERATE(34, 50, 53, 89, 93); // level, IT, levitation, shrink, Pojo
    const auto root = test::scratchDirectory("pojo-named-gameplay");
    const auto bank = [&](std::string_view folder, std::string_view name, s16 sample) {
        test::writeNativeSoundBank(
            root / "AUDIO" / folder,
            std::format(R"({{"sounds":[{{"name":"{}","sequence":[{{"sample":0}}]}}]}})", name),
            std::array{test::NativeSoundSample{48000, std::vector<s16>(48000, sample)}});
    };
    bank("VOICE1", "S_POJO2", -1024);
    bank("CHARACTER", "S_YELWAR2", 1024);
    CharacterSave save;
    save.name = pojo ? "EGG911" : "PLAYER";
    applyNameCheats(save);
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, save, nullptr, {}, 0);
    players[1].actor.spawn(3, {}, nullptr, {}, 0);
    players[0].figure = std::make_unique<PlayerFigure>();
    REQUIRE(players[0].figure->voice().load(root / "AUDIO/CHARACTER"));
    writeTextFile(root / "messages.json", R"({"messages":[
        {"name":"LEVELUP","lines":["LEVEL %d"]},
        {"name":"ISNOWIT","lines":["IT"]},
        {"name":"LEVITATION","lines":["LEVITATION"]},
        {"name":"SHRINKMSG","lines":["SHRINK"]},
        {"name":"POJOMSG","lines":["POJO"]}]})");
    MessageTable messages;
    REQUIRE(messages.load(root / "messages.json"));
    PartyHud hud;
    hud.help().setTexts(&messages);
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    LevelSoundscape audio;
    audio.open(root, &sounds, nullptr);
    REQUIRE(hud.postHelp(lesson, 0, players, audio, 2));
    REQUIRE(sounds.voiceCount() == 1);
    std::array<f32, 2048> samples{};
    mixer.mix(samples);
    CHECK(std::accumulate(samples.begin(), samples.end(), 0.0f) * (pojo ? -1 : 1) > 0);
}

TEST_CASE("shop level announcements retain Pojo's name after saving and loading EGG911",
          "[pojo][pojo-voice][shop][audio][assets]") {
    const bool pojo = GENERATE(false, true);
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    CharacterSave save;
    save.name = pojo ? "EGG911" : "PLAYER";
    save.color = 1;
    save.progress().experience = levelExperience(2);
    save = CharacterSave::fromJson(save.toJson());
    const std::array party{PartyMember{2, save}};
    const std::array results{LevelResults{2, {0, 0, levelExperience(2)}}};
    test::FakeRenderDevice device;
    GameContext context;
    context.unpackedRoot = root;
    AfterLevelScene scene;
    REQUIRE(scene.open(device, context, party, results, {1000, 100, 1000}, "G1"));
    scene.update(10, {});
    ShopSession::Inputs input{};
    input[2].select = true;
    scene.update(0, input);
    REQUIRE(scene.session().lanes()[0].phase == ShopPhase::BeforeStats);
    CHECK(scene.lastSounds() == std::vector<std::string>{"S_OPTMENUSEL",
                                                         pojo ? "S_POJO2" : "S_BLUWAR2", "S_HAS",
                                                         "S_GAINEDLEVEL"});
}
} // namespace
