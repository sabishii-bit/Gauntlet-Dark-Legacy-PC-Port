#include <array>
#include <format>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/players/PowerupEffects.h"
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
        const std::array<test::NativeSoundSample, 1> bankSamples{
            {{48000, {pcm.begin(), pcm.end()}}}};
        test::writeNativeSoundBank(path,
                                   std::format(R"({{"sounds":[{{"index":0,
            "name":"{}","volume":127,"sequence":[{{"sample":0,"loopStart":true,"loopBack":true}}]}}]}})",
                                               name),
                                   bankSamples);
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

TEST_CASE("world-anchored trap lessons use neutral ink and retain party history",
          "[game][screens][party-hud]") {
    const auto root = test::scratchDirectory("hud-trap-lesson");
    writeTextFile(root / "messages.json",
                  R"({"messages":[{"name":"TRAPMOVE","lines":["TRAPS MAKE SOME OBJECTS MOVE"]}]})");
    MessageTable messages;
    REQUIRE(messages.load(root / "messages.json"));
    PartyHud hud;
    hud.help().setTexts(&messages);
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, Vec3{0}, 0);
    REQUIRE(hud.postHelp(HelpMessages::kTrapsMove, 0, players, audio, -1, Vec3{1, 2, 3}));
    REQUIRE(hud.help().player() == -1);
    REQUIRE(players[0].actor.save().helpSeen == std::vector<s32>{5});
    REQUIRE(HelpMessages::specOf(5)->voice == "S_TRAPSMAKE");
    REQUIRE_FALSE(hud.postHelp(5, 0, players, audio, -1, Vec3{1, 2, 3}));
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
    REQUIRE_FALSE(PartyHud::status(2, players).towerPrompt);
    // Fallen out of the tower, the box asks whether to wait there or quit; gone, it is empty.
    players[0].towerPrompt = true;
    REQUIRE(PartyHud::status(2, players).towerPrompt);
    players[0].towerPrompt = false;
    players[0].departed = true;
    REQUIRE_FALSE(PartyHud::status(2, players).active);
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

TEST_CASE("pickup focus and usage are private to each player's controller identity",
          "[game][screens][party-hud][selector][multiplayer]") {
    PartyHud hud;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 2> players;
    for (usize i = 0; i < players.size(); ++i) {
        players[i].actor.spawn(i == 0 ? 3 : 1, {}, nullptr, Vec3{0}, 0);
        Inventory& inventory = players[i].actor.save().progress().inventory;
        inventory.addPowerup(powerup::kSpeed, 0, 5, 12.3f);
        inventory.addPowerup(powerup::kSpecial, powerup::kGrowth, 0, 30);
    }
    hud.focusPickup(players[0].actor, powerup::kSpeed, 0);
    hud.focusPickup(players[1].actor, powerup::kSpecial, powerup::kGrowth);
    REQUIRE(hud.selector(3).selection() == 0);
    REQUIRE(hud.selector(1).selection() == 1);
    REQUIRE(hud.selector(0).selection() == -1);
    REQUIRE(PartyHud::status(3, players, &hud.selector(3)).powerup->strength == 12.3f);
    REQUIRE(PartyHud::status(1, players, &hud.selector(1)).powerup->strength == 30);
    hud.stepSelector(players[0].actor, SelectorInput{.up = true}, 32, audio);
    hud.stepSelector(players[0].actor, {}, 1, audio);
    hud.stepSelector(players[0].actor, SelectorInput{.up = true}, 1, audio);
    REQUIRE_FALSE(players[0].actor.save().progress().inventory.powerups[0].on);
    REQUIRE(players[1].actor.save().progress().inventory.powerups[0].on);
    REQUIRE(PartyHud::status(3, players, &hud.selector(3)).powerup->flags == powerup::kGrowth);
    players[0].life = PlayerLife::Dying;
    REQUIRE_FALSE(PartyHud::status(3, players, &hud.selector(3)).powerup);
    players[1].departed = true;
    REQUIRE_FALSE(PartyHud::status(1, players, &hud.selector(1)).powerup);
}
TEST_CASE("crystal-style item announcements use common audio and do not replay when seen",
          "[game][screens][party-hud][items]") {
    const auto root = test::scratchDirectory("party-hud-item-voice");
    const auto path = root / "audio/COMMON";
    std::filesystem::create_directories(path);
    const std::array<s16, 4> pcm{8192, 8192, 8192, 8192};
    const std::array<test::NativeSoundSample, 1> bankSamples{{{48000, {pcm.begin(), pcm.end()}}}};
    test::writeNativeSoundBank(path, R"({"sounds":[{"name":"S_PICKUPCRYST","volume":127,
        "sequence":[{"sample":0,"loopStart":true,"loopBack":true}]}]})",
                               bankSamples);
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
