#include <array>
#include <format>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/WavWriter.h"
#include "game/screens/PartyHud.h"
namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("level announcement queues gained-level behind the color and character name",
          "[game][screens][party-hud]") {
    const auto root = test::scratchDirectory("party-hud-level-voice");
    const auto bank = [&](std::string_view folder, std::string_view name, s16 value) {
        const auto path = root / "audio" / folder;
        std::filesystem::create_directories(path);
        const std::array<s16, 4> pcm{value, value, value, value};
        writeFile(path / "tone.wav", formats::encodeWav(pcm, 48000, 1));
        writeTextFile(path / "sounds.json", std::format(R"({{"sounds":[{{"index":0,
            "name":"{}","volume":127,"sequence":[{{"sample":0,"loopStart":true,"loopBack":true}}]}}],
            "samples":[{{"index":0,"file":"tone.wav","sampleRate":48000,"frames":4}}]}})",
                                                        name));
    };
    const CharacterSave save;
    const std::string name =
        std::format("S_{}{}2", colorCode(save.color), classCode(save.character));
    bank("CHARACTER", name, 8192);
    bank("VOICE1", "S_GAINEDLEVEL", -8192);
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    LevelSoundscape audio;
    audio.open(root, &sounds, nullptr);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
    players[0].figure = std::make_unique<PlayerFigure>();
    REQUIRE(players[0].figure->voice().load(root / "audio/CHARACTER"));
    PartyHud hud;
    writeTextFile(root / "messages.json",
                  R"({"messages":[{"name":"LEVELUP","lines":["LEVEL %d"]}]})");
    MessageTable messages;
    REQUIRE(messages.load(root / "messages.json"));
    hud.help().setTexts(&messages);
    REQUIRE(hud.postHelp(HelpMessages::kLevelUp, 0, players, audio, 2));
    REQUIRE(sounds.voiceCount() == 1); // The narrator waits, rather than speaking over the name.
    std::array<f32, 128> output{};
    mixer.mix(output);
    REQUIRE(output.back() > 0); // Positive samples identify the color+character bank.
    audio.close();
}

TEST_CASE("party HUD status uses player identity rather than party order",
          "[game][screens][party-hud]") {
    std::array<PlayerRuntime, 2> players;
    CharacterSave save;
    save.name = "THREE";
    save.gold = 42;
    save.progress().health = 700;
    save.progress().inventory.addKeys(2);
    save.progress().inventory.addPotions(3, 2);
    players[0].actor.spawn(3, save, nullptr, Vec3{0}, 0);
    save.name = "ONE";
    players[1].actor.spawn(1, save, nullptr, Vec3{0}, 0);
    const auto status = PartyHud::status(3, players);
    REQUIRE(status.active);
    REQUIRE(status.name == "THREE");
    REQUIRE(status.health == 700);
    REQUIRE(status.keys == 2);
    REQUIRE(status.potions == 2);
    REQUIRE(status.potionKind == 3);
    REQUIRE(status.gold == 42);
    REQUIRE(status.turbo.has_value());
    REQUIRE(PartyHud::status(1, players).name == "ONE");
    REQUIRE_FALSE(PartyHud::status(0, players).active);
    REQUIRE_FALSE(PartyHud::status(-1, players).active);
}

TEST_CASE("party HUD hides carried goods and health while a character falls",
          "[game][screens][party-hud]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, Vec3{0}, 0);
    players[0].actor.save().progress().health = 1;
    players[0].actor.save().progress().inventory.addKeys(5);
    players[0].life = PlayerLife::Dying;
    const auto dying = PartyHud::status(2, players);
    REQUIRE(dying.active);
    REQUIRE(dying.health == 0);
    REQUIRE(dying.keys == 0);
    REQUIRE_FALSE(dying.inTower);
    REQUIRE_FALSE(dying.turbo.has_value());
    players[0].life = PlayerLife::InTower;
    REQUIRE(PartyHud::status(2, players).inTower);
}

TEST_CASE("party HUD clears transient presentation without altering participants",
          "[game][screens][party-hud]") {
    PartyHud hud;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    players[0].actor.save().progress().health = 123;
    REQUIRE_FALSE(hud.postHelp(0, 9, players, audio));
    hud.pickups().addCard(3, "GOLD");
    hud.clear();
    hud.clear();
    REQUIRE_FALSE(hud.help().showing());
    REQUIRE_FALSE(hud.selector(3).showing());
    REQUIRE(players[0].actor.save().health() == 123);
}
TEST_CASE("crystal-style item announcements use common audio and do not replay when seen",
          "[game][screens][party-hud][items]") {
    const auto root = test::scratchDirectory("party-hud-item-voice");
    const auto path = root / "audio/COMMON";
    std::filesystem::create_directories(path);
    const std::array<s16, 4> pcm{8192, 8192, 8192, 8192};
    writeFile(path / "tone.wav", formats::encodeWav(pcm, 48000, 1));
    writeTextFile(path / "sounds.json", R"({"sounds":[{"name":"S_PICKUPCRYST","volume":127,
        "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]}],
        "samples":[{"index":0,"file":"tone.wav","sampleRate":48000,"frames":4}]})");
    writeTextFile(root / "messages.json", R"({"messages":[{"name":"MIKEY","lines":["MIKEY"]}]})");
    MessageTable messages;
    REQUIRE(messages.load(root / "messages.json"));
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    LevelSoundscape audio;
    audio.open(root, &sounds, nullptr);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    PartyHud hud;
    hud.help().setTexts(&messages);
    REQUIRE(hud.postHelp(148, 0, players, audio));
    REQUIRE(sounds.voiceCount() == 1);
    std::array<f32, 128> output{};
    mixer.mix(output);
    REQUIRE(output.back() > 0);
    hud.help().update(1000);
    REQUIRE_FALSE(hud.postHelp(148, 0, players, audio));
    REQUIRE(sounds.voiceCount() == 1);
    audio.close();
}
} // namespace
