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
#include "game/config/GameConfig.h"
#include "game/players/ItemPickup.h"
#include "game/players/NameCheats.h"
#include "game/screens/PartyPickups.h"
#include "game/screens/PlayScene.h"
#include "game/screens/PlayerPowerups.h"
#include "game/world/LevelCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a native Turbo Boost pickup is carried until its owner activates the selector",
          "[pickups][turbo-activation][assets][multiplayer]") {
    const auto root =
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
    const PartyPickups::Services services{.world = world,
                                          .fixtures = fixtures,
                                          .hud = hud,
                                          .audio = audio,
                                          .classes = classes,
                                          .sounds = nullptr,
                                          .help = {},
                                          .openMessage = {},
                                          .challengeCoin = {}};
    std::array<PlayerRuntime, 2> players;
    const Vec3 spot{10.7f, 10.2f, -60.5f};
    players[0].actor.spawn(3, {}, nullptr, spot, 0);
    players[1].actor.spawn(1, {}, nullptr, spot + Vec3{40, 0, 0}, 0);
    REQUIRE(world.placeItem(device, "TURBOPUP", spot));
    const usize item = world.placedItems().size() - 1;
    PartyPickups pickups;
    pickups.collect(device, players, services);
    CHECK_FALSE(world.placedItems().item(item).visible);
    auto& inventory = players[0].actor.save().progress().inventory;
    REQUIRE(inventory.powerupCount() == 1);
    REQUIRE(inventory.powerups[0].flags == powerup::kTurbo);
    CHECK_FALSE(inventory.powerups[0].on);
    CHECK(players[0].turbo.held() == 0);
    CHECK(hud.selector(3).selection() == 0);
    CHECK_FALSE(hud.selector(3).showing());
    CHECK(hud.selector(1).selection() == -1);
    PlayerPowerups::update(players, 60, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 0);
    REQUIRE(inventory.powerupCount() == 1);
    hud.stepSelector(players[0].actor, SelectorInput{.up = true}, 32, audio);
    hud.stepSelector(players[0].actor, {}, 1, audio);
    REQUIRE(hud.selector(3).showing());
    CHECK_FALSE(inventory.powerups[0].on); // Opening is not activation.
    hud.stepSelector(players[0].actor, SelectorInput{.up = true}, 1, audio);
    REQUIRE(inventory.powerups[0].on);
    PlayerPowerups::update(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 100);
    CHECK(inventory.powerupCount() == 0);
    REQUIRE(players[0].turbo.spend(40));
    PlayerPowerups::update(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 60);
    CHECK_FALSE(inventory.powerups[0].on);
    REQUIRE(world.placeItem(device, "TURBOPUP", spot));
    pickups.collect(device, players, services);
    REQUIRE(inventory.powerupCount() == 1);
    CHECK_FALSE(inventory.powerups[0].on);
    PlayerPowerups::update(players, 0.01f, PlayerPowerups::Clock::Level);
    CHECK(players[0].turbo.held() == 60); // Collecting again must not reuse the old activation.
    CHECK(inventory.powerupCount() == 1);
    CHECK(players[1].turbo.held() == 0);
    CHECK(players[1].actor.save().progress().inventory.powerupCount() == 0);
}

TEST_CASE("world pickups honor auto use and remain available in the selector",
          "[pickups][combat-settings][assets]") {
    const bool activate = GENERATE(false, true);
    const auto root =
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
    PartyPickups::Services services{.world = world,
                                    .fixtures = fixtures,
                                    .hud = hud,
                                    .audio = audio,
                                    .classes = classes,
                                    .sounds = nullptr,
                                    .help = {},
                                    .openMessage = {},
                                    .challengeCoin = {}};
    services.autoActivateItems = activate;
    std::array<PlayerRuntime, 1> players;
    const Vec3 spot{10.7f, 10.2f, -60.5f};
    players[0].actor.spawn(3, {}, nullptr, spot, 0);
    const auto& records = world.layout().itemInfos();
    const auto found = std::ranges::find_if(records, [](const auto& record) {
        return record.type == 1 && record.subtype == static_cast<s32>(ItemKind::WeaponPowerup);
    });
    REQUIRE(found != records.end());
    REQUIRE(world.placeItem(device, found->name, spot));
    PartyPickups pickups;
    pickups.collect(device, players, services);
    auto& inventory = players[0].actor.save().progress().inventory;
    REQUIRE(inventory.powerupCount() == 1);
    CHECK(inventory.powerups[0].on == activate);
    CHECK(hud.selector(3).selection() == 0);
    hud.stepSelector(players[0].actor, SelectorInput{.up = true}, 32, audio);
    hud.stepSelector(players[0].actor, {}, 1, audio);
    hud.stepSelector(players[0].actor, SelectorInput{.up = true}, 1, audio);
    CHECK(inventory.powerups[0].on == !activate);
}

TEST_CASE("Stop Time pickups honor the coin-stage restriction without being consumed",
          "[pickups][secret][stop-time][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    const bool secret = GENERATE(false, true);
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    // Retail coin stages have no Stop Time pickup. Treat the native town layout
    // as a modded coin stage to test the collection path with real pickup data.
    auto level = *levels.byName("G1");
    if (secret) {
        level.realmId = LevelRef::kSecretRealm;
    }
    LevelWorld world;
    REQUIRE(world.load(device, root, level));
    LevelFixtures fixtures;
    PartyHud hud;
    LevelSoundscape audio;
    const ClassDataSet classes;
    const PartyPickups::Services services{.world = world,
                                          .fixtures = fixtures,
                                          .hud = hud,
                                          .audio = audio,
                                          .classes = classes,
                                          .help = {},
                                          .openMessage = {},
                                          .challengeCoin = {},
                                          .autoActivateItems = true};
    std::array<PlayerRuntime, 1> players;
    const Vec3 spot{1000, 1000, 1000}; // Isolate this pickup from the stage's authored coins.
    players[0].actor.spawn(3, {}, nullptr, spot, 0);
    const auto& records = world.layout().itemInfos();
    const auto found = std::ranges::find_if(records, [](const auto& record) {
        return record.type == 1 && record.subtype == powerup::kSpecial &&
               (record.properties & powerup::kStopTime) != 0;
    });
    REQUIRE(found != records.end());
    REQUIRE(world.placeItem(device, found->name, spot));
    PartyPickups pickups;
    pickups.collect(device, players, services);
    auto& inventory = players[0].actor.save().progress().inventory;
    REQUIRE(inventory.powerupCount() == 1);
    CHECK(inventory.powerups[0].on == !world.ref().isSecret());
    CHECK(inventory.powerups[0].held());
    CHECK(hud.selector(3).selection() == 0);
    const f32 duration = inventory.powerups[0].strength;
    PlayerPowerups::update(players, 1, PlayerPowerups::Clock::Level);
    CHECK(inventory.powerups[0].strength == (world.ref().isSecret() ? duration : duration - 1));
}

TEST_CASE("tower crystal congratulations survive the in-level notice and acknowledge each class",
          "[pickups][tower-crystals][multiplayer]") {
    LevelWorld world;
    LevelFixtures fixtures;
    PartyHud hud;
    LevelSoundscape audio;
    const ClassDataSet classes;
    std::array<PlayerRuntime, 2> players;
    std::vector<usize> pages;
    bool canOpen = true;
    PartyPickups::Services services{.world = world,
                                    .fixtures = fixtures,
                                    .hud = hud,
                                    .audio = audio,
                                    .classes = classes,
                                    .sounds = nullptr,
                                    .help = {},
                                    .openMessage = {},
                                    .challengeCoin = {}};
    services.openMessage = [&](std::string_view key, usize page) {
        CHECK(key == "UNLOCKLEVEL");
        if (canOpen) {
            pages.push_back(page);
        }
        return canOpen;
    };
    constexpr usize kRealm = 2;
    const s32 needed = LevelTriggers::crystalsNeeded(kRealm);
    players[0].actor.save().character = 6;
    auto& complete = players[0].actor.save().progress();
    complete.crystals[kRealm] = needed;
    complete.unlocked = 1U << kRealm; // Already displayed the in-level notice.
    players[1].actor.save().progress().crystals[kRealm] = needed - 1;
    SECTION("a completed party member suppresses a repeated ceremony") {
        players[1].actor.save().progress().crystals[kRealm] = -1;
        CHECK_FALSE(PartyPickups::announceTowerUnlock(players, services));
        CHECK(pages.empty());
        return;
    }
    SECTION("departed members cannot suppress the returning party") {
        players[1].actor.save().progress().crystals[kRealm] = -1;
        players[1].departed = true;
    }
    SECTION("a refused scroll does not consume the acknowledgement") {
        canOpen = false;
        CHECK_FALSE(PartyPickups::announceTowerUnlock(players, services));
        CHECK(complete.crystals[kRealm] == needed);
        canOpen = true;
    }
    const s32 otherCount = players[1].actor.save().progress().crystals[kRealm];
    REQUIRE(PartyPickups::announceTowerUnlock(players, services));
    CHECK(pages == std::vector<usize>{kRealm});
    CHECK(complete.crystals[kRealm] == -1);
    CHECK(players[0].entrySave.classes[6].crystals[kRealm] == -1);
    CHECK(players[0].actor.save().classes[0].crystals[kRealm] == 0);
    CHECK(players[1].actor.save().progress().crystals[kRealm] == otherCount);
    CHECK(CharacterSave::fromJson(players[0].actor.save().toJson()).progress().crystals[kRealm] ==
          -1);
    CHECK_FALSE(PartyPickups::announceTowerUnlock(players, services));
    CHECK(pages.size() == 1);
}

TEST_CASE("the narrator counts the party's runestones as the original does",
          "[game][screens][pickups]") {
    CHECK(PartyPickups::runeCountVoices(0).empty());
    CHECK(PartyPickups::runeCountVoices(1) == std::vector<std::string>{"S_RUNEFOUND1"});
    CHECK(PartyPickups::runeCountVoices(2) == std::vector<std::string>{"S_RUNE2", "S_RUNEFOUND2"});
    CHECK(PartyPickups::runeCountVoices(12) ==
          std::vector<std::string>{"S_RUNE12", "S_RUNEFOUND2"});
    CHECK(PartyPickups::runeCountVoices(13).empty()); // AudioNumRunesFound has no thirteenth
}

TEST_CASE("returning to the tower opens the pending crystal scroll after arrival",
          "[tower-crystals][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("L1")));
    GameConfig config;
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.levels = &catalog;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "TEST";
    save.progress().crystals[2] = 100;
    save.progress().unlocked = 1U << 2;
    PlayOptions options;
    options.welcome = false;
    const std::array party{PartyMember{0, save}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.actor(0));
    for (s32 frame = 0; frame < 1800 && !scene.scroll().active(); ++frame) {
        REQUIRE(scene.update(1.0 / 60, {}) == PlayOutcome::Running);
        if (scene.spawning()) {
            REQUIRE(scene.actor(0)->save().progress().crystals[2] == 100);
        }
    }
    REQUIRE(scene.scroll().active());
    CHECK(scene.actor(0)->save().progress().crystals[2] == -1);
}

TEST_CASE("collecting the last orange crystal congratulates a new character only once",
          "[pickups][tower-crystals][assets]") {
    const auto* name = GENERATE("TEST", "EGG911");
    const s32 otherClassCrystals = GENERATE(0, 15, -1);
    CAPTURE(name, otherClassCrystals);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("L1")));
    const GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.sounds = &sounds;
    context.levels = &catalog;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = name;
    applyNameCheats(save);
    save.classes[1].crystals[1] = otherClassCrystals;
    save.progress().crystals[1] = 14;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{19.3f, -2.0f, -62.0f}; // One of the native orange crystals.
    PlayScene scene;
    const std::array party{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, party, options));
    for (s32 frame = 0; frame < 600 && !scene.scroll().active(); ++frame) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
    }
    REQUIRE(scene.scroll().active());
    CHECK(std::ranges::any_of(scene.scroll().lines(), [](const std::string& line) {
        return line.find("Congratulations") != std::string::npos;
    }));
    const SoundHandle voice = scene.voice();
    REQUIRE(voice != kNoSound);
    REQUIRE(sounds.isPlaying(voice));
    PlayScene::Inputs accept;
    accept[0].menu.select = true;
    for (s32 frame = 0; frame < 300 && scene.scroll().active(); ++frame) {
        REQUIRE(scene.update(1.0 / 30, frame % 20 == 19 ? accept : PlayScene::Inputs{}) ==
                PlayOutcome::Running);
    }
    REQUIRE_FALSE(scene.scroll().active());
    CHECK_FALSE(sounds.isPlaying(voice));
    // Wait beyond the Tower's three-second check, without dismissing a second message.
    for (s32 frame = 0; frame < 240; ++frame) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
    }
    CHECK_FALSE(scene.scroll().active());
    CHECK(scene.voice() == kNoSound);
    CHECK(scene.actor(0)->save().progress().crystals[1] == -1);
    CHECK(scene.actor(0)->save().classes[1].crystals[1] == otherClassCrystals);
    CHECK(scene.participants()[0].entrySave.progress().crystals[1] == -1);
    // Reload the acknowledged character into a new visit; other classes stay independent.
    save = CharacterSave::fromJson(scene.actor(0)->save().toJson());
    scene.close();
    const std::array returning{PartyMember{0, save}};
    REQUIRE(scene.open(device, context, world, returning, options));
    for (s32 frame = 0; frame < 240; ++frame) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
    }
    CHECK_FALSE(scene.scroll().active());
    CHECK(scene.voice() == kNoSound);
    CHECK(scene.actor(0)->save().classes[1].crystals[1] == otherClassCrystals);
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
