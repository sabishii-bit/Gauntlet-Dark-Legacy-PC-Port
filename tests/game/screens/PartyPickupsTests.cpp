#include <algorithm>
#include <array>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/players/ItemPickup.h"
#include "game/screens/PartyPickups.h"
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the narrator counts the party's runestones as the original does",
          "[game][screens][pickups]") {
    CHECK(PartyPickups::runeCountVoices(0).empty());
    CHECK(PartyPickups::runeCountVoices(1) == std::vector<std::string>{"S_RUNEFOUND1"});
    CHECK(PartyPickups::runeCountVoices(2) == std::vector<std::string>{"S_RUNE2", "S_RUNEFOUND2"});
    CHECK(PartyPickups::runeCountVoices(12) ==
          std::vector<std::string>{"S_RUNE12", "S_RUNEFOUND2"});
    CHECK(PartyPickups::runeCountVoices(13).empty()); // AudioNumRunesFound has no thirteenth
}

TEST_CASE("the party's pickups are shared, taught, gestured and handed to the scene",
          "[game][screens][pickups][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    LevelFixtures fixtures;
    PartyHud hud;
    LevelSoundscape audio;
    const ClassDataSet classes;
    std::array<PlayerRuntime, 2> players;
    const Vec3 spot{10.7f, 10.2f, -60.5f}; // open ground
    CharacterSave second;
    second.character = 1;
    players[0].actor.spawn(0, CharacterSave{}, nullptr, spot, 0.0f);
    players[1].actor.spawn(1, second, nullptr, spot + Vec3{40.0f, 0.0f, 0.0f}, 0.0f);
    std::vector<std::pair<s32, usize>> lessons;
    std::vector<usize> coins;
    std::vector<std::pair<std::string, usize>> messages;
    PartyPickups pickups;
    const PartyPickups::Services services{
        .world = world,
        .fixtures = fixtures,
        .hud = hud,
        .audio = audio,
        .classes = classes,
        .sounds = nullptr,
        .help =
            [&](s32 id, usize index) {
                lessons.emplace_back(id, index);
                return id == 95; // the first two potion lessons already told
            },
        .openMessage =
            [&](std::string_view name, usize index) {
                messages.emplace_back(name, index);
                return false;
            },
        .challengeCoin = [&](usize item) { coins.push_back(item); }};

    SECTION("a runestone is everyone's, whoever stood on it") {
        REQUIRE(world.placeItem(device, "RUNEC2", spot));
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().progress().relics.hasRune(7));
        CHECK(players[1].actor.save().progress().relics.hasRune(7));
        CHECK(players[0].gesture == PlayerDeed::None); // runestones are not stooped for
    }
    SECTION("gold goes to the challenge and is stooped for out of the tower") {
        REQUIRE(world.placeItem(device, "TREAS_GOLD", spot));
        const usize item = world.placedItems().size() - 1;
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().gold > 0);
        CHECK(coins == std::vector<usize>{item});
        CHECK(players[0].gesture == PlayerDeed::Pick);
        CHECK(hud.pickups().cards().size() == 1);
    }
    SECTION("a potion tries its lessons in turn until one is told") {
        REQUIRE(world.placeItem(device, "POT_GRE", spot));
        pickups.collect(device, players, services);
        REQUIRE(lessons.size() >= 3);
        CHECK(lessons[0] == std::pair<s32, usize>{7, 0});
        CHECK(lessons[1] == std::pair<s32, usize>{94, 0});
        CHECK(lessons[2] == std::pair<s32, usize>{95, 0});
    }
    SECTION("a powerup pickup focuses only the taker's selector even when renewing an old slot") {
        players[0].actor.spawn(3, {}, nullptr, spot, 0);
        const auto& records = world.layout().itemInfos();
        const auto record = std::ranges::find_if(records, [](const ItemInfo& info) {
            return info.type == ItemInfo::kPowerup &&
                   info.subtype == static_cast<s32>(ItemKind::SpeedPowerup);
        });
        REQUIRE(record != records.end());
        const auto recordIndex = static_cast<s32>(std::distance(records.begin(), record));
        REQUIRE(world.placeItemRecord(device, recordIndex, spot, 20));
        pickups.collect(device, players, services);
        REQUIRE(hud.selector(3).selection() == 0);
        REQUIRE(hud.selector(3).state() == PowerupSelector::State::Closed);
        REQUIRE(hud.selector(1).selection() == -1);
        Inventory& inventory = players[0].actor.save().progress().inventory;
        inventory.addPowerup(static_cast<s32>(ItemKind::SpecialPowerup), 4, 0, 60);
        hud.focusPickup(players[0].actor, static_cast<s32>(ItemKind::SpecialPowerup), 4);
        REQUIRE(hud.selector(3).selection() == 1);
        REQUIRE(world.placeItemRecord(device, recordIndex, spot, 20));
        pickups.collect(device, players, services);
        REQUIRE(hud.selector(3).selection() == 0);
        REQUIRE(hud.selector(1).selection() == -1);
    }
    SECTION("the fallen reach nothing") {
        players[0].life = PlayerLife::InTower;
        REQUIRE(world.placeItem(device, "TREAS_GOLD", spot));
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().gold == 0);
        CHECK(world.placedItems().item(world.placedItems().size() - 1).visible);
    }
    SECTION("departed slots neither collect nor share a runestone") {
        players[0].departed = true;
        REQUIRE(world.placeItem(device, "TREAS_GOLD", spot));
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().gold == 0);
        CHECK(world.placedItems().item(world.placedItems().size() - 1).visible);
        REQUIRE(world.placeItem(device, "RUNEC2", players[1].actor.position()));
        pickups.collect(device, players, services);
        CHECK_FALSE(players[0].actor.save().progress().relics.hasRune(7));
        CHECK(players[1].actor.save().progress().relics.hasRune(7));
    }
    SECTION("the fallen keep their old rune in the narration but receive no new rune") {
        players[1].life = PlayerLife::Dying;
        SECTION("while the death animation plays") {}
        SECTION("while waiting in the tower") {
            players[1].life = PlayerLife::InTower;
        }
        players[1].actor.save().progress().relics.addRune(0);
        const auto audioRoot = test::scratchDirectory("pickup-fallen-rune-voices");
        const std::array<test::NativeSoundSample, 1> samples{{{48000, std::vector<s16>(48000)}}};
        test::writeNativeSoundBank(audioRoot / "audio/VOICE1", R"({"sounds":[
            {"index":0,"name":"S_RUNE2","duration":1,"sequence":[{"sample":0}]},
            {"index":1,"name":"S_RUNEFOUND2","duration":1,"sequence":[{"sample":0}]}]})",
                                   samples);
        AudioMixer mixer(48000);
        SoundPlayer output(mixer);
        audio.open(audioRoot, &output, nullptr);
        REQUIRE(world.placeItem(device, "RUNEC2", spot));
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().progress().relics.hasRune(7));
        CHECK_FALSE(players[1].actor.save().progress().relics.hasRune(7));
        CHECK(players[1].actor.save().progress().relics.hasRune(0));
        // Only the two-rune announcement exists in the bank: losing the fallen player's
        // existing rune would attempt the absent single-rune voice and enqueue nothing.
        CHECK(audio.narrationBacklog() > 1.5);
        audio.close(); // release the borrowed output before its mixer is destroyed
    }
    SECTION("a crystal gate uses the best participant rather than the least advanced") {
        REQUIRE(world.placeItem(device, "GEMBLUE", spot));
        const auto realm =
            static_cast<usize>(world.placedItems().item(world.placedItems().size() - 1).realm());
        REQUIRE(realm < kRealmCount);
        const s32 wanted = LevelTriggers::crystalsNeeded(static_cast<s32>(realm));
        REQUIRE(wanted > 1);
        players[0].actor.save().progress().crystals[realm] = wanted - 1;
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().progress().crystals[realm] == wanted);
        CHECK(players[1].actor.save().progress().crystals[realm] == 1);
        CHECK(messages == std::vector<std::pair<std::string, usize>>{{"UNLOCKLEVEL", realm}});
    }
    SECTION("a completed crystal record stays completed and does not announce again") {
        REQUIRE(world.placeItem(device, "GEMBLUE", spot));
        const auto realm =
            static_cast<usize>(world.placedItems().item(world.placedItems().size() - 1).realm());
        REQUIRE(realm < kRealmCount);
        players[0].actor.save().progress().crystals[realm] = -1;
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().progress().crystals[realm] == -1);
        CHECK(players[1].actor.save().progress().crystals[realm] == 1);
        CHECK(messages.empty());
    }
    SECTION("fallen crystal records count toward access but receive no new crystal") {
        players[1].life = PlayerLife::Dying;
        SECTION("during the death animation") {}
        SECTION("while waiting in the tower") {
            players[1].life = PlayerLife::InTower;
        }
        REQUIRE(world.placeItem(device, "GEMBLUE", spot));
        const auto realm =
            static_cast<usize>(world.placedItems().item(world.placedItems().size() - 1).realm());
        REQUIRE(realm < kRealmCount);
        const s32 wanted = LevelTriggers::crystalsNeeded(static_cast<s32>(realm));
        REQUIRE(wanted > 1);
        players[0].actor.save().progress().crystals[realm] = wanted - 1;
        players[1].actor.save().progress().crystals[realm] = wanted;
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().progress().crystals[realm] == wanted);
        CHECK(players[1].actor.save().progress().crystals[realm] == wanted);
        CHECK(messages.empty()); // their already qualified record suppresses a repeat

        players[1].actor.save().progress().crystals[realm] = 0;
        REQUIRE(world.placeItem(device, "GEMBLUE", spot));
        pickups.collect(device, players, services);
        CHECK(players[1].actor.save().progress().crystals[realm] == 0);
    }
    SECTION("a departed player's crystal record does not suppress a new gate announcement") {
        REQUIRE(world.placeItem(device, "GEMBLUE", spot));
        const auto realm =
            static_cast<usize>(world.placedItems().item(world.placedItems().size() - 1).realm());
        REQUIRE(realm < kRealmCount);
        const s32 wanted = LevelTriggers::crystalsNeeded(static_cast<s32>(realm));
        REQUIRE(wanted > 0);
        players[0].actor.save().progress().crystals[realm] = wanted - 1;
        players[1].actor.save().progress().crystals[realm] = -1;
        players[1].departed = true;
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().progress().crystals[realm] == wanted);
        CHECK(players[1].actor.save().progress().crystals[realm] == -1);
        CHECK(messages == std::vector<std::pair<std::string, usize>>{{"UNLOCKLEVEL", realm}});
        CHECK(players[1].actor.save().progress().unlocked == 0);
    }
}

TEST_CASE("the chest opener complains only about retail theft eligible pickups",
          "[game][screens][pickups][multiplayer][assets]") {
    const auto [kind, amount, keys, complains] = GENERATE(
        std::tuple{ItemKind::Gold, 10, 0, false}, std::tuple{ItemKind::Gold, 24, 0, false},
        std::tuple{ItemKind::Gold, 25, 0, true}, std::tuple{ItemKind::Keys, 3, 8, false},
        std::tuple{ItemKind::Keys, 2, 7, true}, std::tuple{ItemKind::Food, 100, 0, true},
        std::tuple{ItemKind::Potion, 1, 0, true}, std::tuple{ItemKind::Runestone, 7, 0, false},
        std::tuple{ItemKind::SpeedPowerup, 20, 0, false});
    CAPTURE(kind, amount, keys);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    const Vec3 spot{10.7f, 10.2f, -60.5f};
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, spot + Vec3{40, 0, 0}, 0);
    players[1].actor.spawn(1, {}, nullptr, spot, 0);
    players[1].actor.save().progress().health = 100;
    players[1].actor.save().progress().inventory.keys = keys;

    const auto voiceRoot = test::scratchDirectory("pickup-theft-voice");
    const auto voiceBank = voiceRoot / "audio/WAR";
    const std::array<test::NativeSoundSample, 1> samples{{{48000, std::vector<s16>(48000)}}};
    test::writeNativeSoundBank(voiceBank, R"({"sounds":[
        {"index":0,"name":"S_WARSTEAL","duration":1,"sequence":[{"sample":0}]}]})",
                               samples);
    players[0].figure = std::make_unique<PlayerFigure>();
    REQUIRE(players[0].figure->voice().load(voiceBank));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    LevelSoundscape audio;
    audio.open(voiceRoot, &output, nullptr);
    LevelFixtures fixtures;
    PartyHud hud;
    const ClassDataSet classes;
    const PartyPickups::Services services{.world = world,
                                          .fixtures = fixtures,
                                          .hud = hud,
                                          .audio = audio,
                                          .classes = classes,
                                          .sounds = nullptr,
                                          .help = {},
                                          .openMessage = {},
                                          .challengeCoin = {}};
    const auto& records = world.layout().itemInfos();
    const auto record = std::ranges::find_if(records, [&](const ItemInfo& info) {
        return info.type == ItemInfo::kPowerup && info.subtype == static_cast<s32>(kind) &&
               (kind != ItemKind::Gold || info.name == "TREAS_GOLD");
    });
    REQUIRE(record != records.end());
    REQUIRE(world.placeItemRecord(device, static_cast<s32>(std::distance(records.begin(), record)),
                                  spot, amount));
    const usize item = world.placedItems().size() - 1;
    world.setItemOpener(item, 3); // Controller id, not vector index zero.
    PartyPickups pickups;
    pickups.collect(device, players, services);
    CHECK((audio.barkBacklog() > 0) == complains);
    if (kind == ItemKind::Keys) {
        CHECK(players[1].actor.save().progress().inventory.keys == 9);
        CHECK(world.placedItems().item(item).taken == (keys + amount <= 9));
    } else {
        CHECK(world.placedItems().item(item).taken);
    }
    audio.close();
}

TEST_CASE("golden idol pieces reward every living participant once and show personal counters",
          "[pickups][multiplayer][golden-idol][assets]") {
    const s32 kind = GENERATE(0, 1, 2);
    const PlayerLife fallen = GENERATE(PlayerLife::Dying, PlayerLife::InTower);
    CAPTURE(kind, fallen);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    const auto& records = world.layout().itemInfos();
    const auto record = std::ranges::find_if(records, [&](const ItemInfo& info) {
        return info.type == ItemInfo::kPowerup &&
               info.subtype == static_cast<s32>(ItemKind::GargoyleKey) && info.value == kind;
    });
    REQUIRE(record != records.end());
    const auto recordIndex = static_cast<s32>(std::distance(records.begin(), record));
    const Vec3 spot{10.7f, 10.2f, -60.5f};
    std::array<PlayerRuntime, 4> players;
    const std::array<s32, 4> slots{1, 3, 0, 2};
    const auto piece = static_cast<usize>(kind);
    const s32 total = Relics::kGargoyleNeeded[piece];
    for (usize i = 0; i < players.size(); ++i) {
        players[i].actor.spawn(slots[i], {}, nullptr, i == 1 ? spot : spot + Vec3{40, 0, 0}, 0);
        players[i].actor.save().progress().relics.gargoylePieces[piece] =
            i == 0 ? total - 1 : static_cast<s32>(i) + 4;
    }
    players[2].life = fallen;
    players[3].departed = true;
    LevelFixtures fixtures;
    PartyHud hud;
    LevelSoundscape audio;
    const ClassDataSet classes;
    const PartyPickups::Services services{.world = world,
                                          .fixtures = fixtures,
                                          .hud = hud,
                                          .audio = audio,
                                          .classes = classes,
                                          .sounds = nullptr,
                                          .help = {},
                                          .openMessage = {},
                                          .challengeCoin = {}};
    PartyPickups pickups;
    constexpr std::array kIcons{"SM_FANGS", "SM_FEATHERS", "SM_CLAWS"};
    for (s32 collected = 1; collected <= 2; ++collected) {
        REQUIRE(world.placeItemRecord(device, recordIndex, spot));
        pickups.collect(device, players, services);
        REQUIRE(world.placedItems().item(world.placedItems().size() - 1).taken);
        CHECK(players[0].actor.save().progress().relics.gargoylePieces[piece] == total);
        CHECK(players[1].actor.save().progress().relics.gargoylePieces[piece] == 5 + collected);
        CHECK(players[2].actor.save().progress().relics.gargoylePieces[piece] == 6);
        CHECK(players[3].actor.save().progress().relics.gargoylePieces[piece] == 7);
        for (usize i = 0; i < 2; ++i) {
            const auto& count = hud.pickups().count(slots[i]);
            CHECK(count.showing());
            CHECK(count.icon == kIcons[piece]);
            CHECK(count.count == (i == 0 ? total : 5 + collected));
            CHECK(count.total == total);
            CHECK(count.secondsLeft == PickupHud::kCountSeconds);
        }
        CHECK_FALSE(hud.pickups().count(slots[2]).showing());
        CHECK_FALSE(hud.pickups().count(slots[3]).showing());
    }
    REQUIRE(hud.pickups().cards().size() == 2);
    for (const auto& card : hud.pickups().cards()) {
        CHECK(card.player == 3);
        CHECK(card.texture == "GOLDNICON");
    }
}

} // namespace
