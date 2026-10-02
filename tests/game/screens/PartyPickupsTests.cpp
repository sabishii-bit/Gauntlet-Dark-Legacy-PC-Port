#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
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
        .openMessage = [](std::string_view, usize) { return false; },
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
    SECTION("the fallen reach nothing") {
        players[0].life = PlayerLife::InTower;
        REQUIRE(world.placeItem(device, "TREAS_GOLD", spot));
        pickups.collect(device, players, services);
        CHECK(players[0].actor.save().gold == 0);
        CHECK(world.placedItems().item(world.placedItems().size() - 1).visible);
    }
}

} // namespace
