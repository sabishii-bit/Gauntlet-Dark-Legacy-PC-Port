#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "DrawStreamDigest.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Inventory.h"
#include "game/characters/Party.h"
#include "game/combat/PowerupFlags.h"
#include "game/config/GameConfig.h"
#include "game/context/GameServices.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayerRuntime.h"
#include "game/scenes/PlayScene.h"

namespace {

using namespace gdl;
using namespace gdl::game;

bool awaitingEntrance(const PlayScene& scene) {
    if (scene.spawning()) {
        return true;
    }
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (scene.animator(player) != nullptr && scene.animator(player)->entering()) {
            return true;
        }
    }
    return false;
}

TEST_CASE("a G1 scene's HUD overlay stays byte for byte what it was",
          "[play-scene][hud-draw-pin][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2");
    GameConfig config;
    // Bloom runs after the world and before the HUD, so where it ran the overlay begins.
    config.display.bloom = true;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", config.text.language));
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameServices context;
    context.config = &config;
    context.strings = &strings;
    context.levels = &levels;
    context.unpackedRoot = root;

    // Four players: seat 0 holds the temple's shard, which shows the runestone finder, and
    // seat 1 carries a powerup it can name.
    constexpr std::array<std::string_view, 4> kNames{"AB", "CD", "EF", "GH"};
    std::vector<PartyMember> party;
    for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
        CharacterSave save;
        save.name = std::string(kNames[static_cast<usize>(seat)]);
        save.character = seat + 2;
        save.color = (seat + 1) % PlayScene::kPlayerCount;
        save.progress().health = 4000;
        party.push_back({seat, save});
    }
    REQUIRE(party[0].save.progress().relics.addShard(9));
    party[1].save.progress().inventory.powerups[0] = PowerupSlot{-1, powerup::kSpeed, 5, 0, false};
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{10.7f, 10.2f, -60.5f}; // open ground
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const PlayScene::Inputs still{};
    const auto play = [&](s32 ticks, const PlayScene::Inputs& inputs) {
        for (s32 tick = 0; tick < ticks; ++tick) {
            scene.update(1.0 / 60.0, inputs);
        }
    };
    for (s32 tick = 0; tick < 600 && awaitingEntrance(scene); ++tick) {
        play(1, still);
    }
    REQUIRE_FALSE(awaitingEntrance(scene));

    // Seat 3 falls and is asked to wait in the tower or quit.
    REQUIRE(scene.kill(3));
    for (s32 tick = 0; tick < 600 && !scene.runtime(3)->towerPrompt; ++tick) {
        play(1, still);
    }
    REQUIRE(scene.runtime(3)->towerPrompt);
    // Seat 1 names its powerup over its box.
    PlayScene::Inputs up{};
    up[1].selector.up = true;
    play(1, up);
    play(40, still);
    REQUIRE(scene.hud().selector(1).showing());
    // A gargoyle's piece underfoot: a card over its taker's box, and each standing player's
    // count of the eagle's pieces.
    REQUIRE(world.placeItem(device, "GARGEAGL", scene.actor(0)->position()));
    for (s32 tick = 0; tick < 120 && scene.pickups().cards().empty(); ++tick) {
        play(1, still);
    }
    REQUIRE(scene.pickups().cards().size() == 1);
    play(20, still);
    // Seat 2 reaches for a potion it has not got, and is told so over its head.
    PlayScene::Inputs reach{};
    reach[2].usePotion = true;
    play(1, reach);
    play(5, still);
    REQUIRE(scene.help().showing());
    REQUIRE(scene.help().player() == 2);
    REQUIRE(scene.pickups().cards().size() == 1);
    REQUIRE(scene.pickups().counts(0)[0].showing());
    REQUIRE(scene.runeMeter().visible());

    device.draws.clear();
    device.bloomDrawOffsets.clear();
    scene.render(device, makeScreenProjection(640, 448), 640, 448);
    REQUIRE(device.bloomDrawOffsets.size() == 1);
    const auto overlay = std::span{device.draws}.subspan(device.bloomDrawOffsets.front());
    REQUIRE_FALSE(overlay.empty());
    // SHA-256 of the overlay's draws, which the scene keeps byte for byte while the HUD's
    // code moves: a literal moves only when what the HUD draws changes on purpose.
    CHECK(test::hexBytes(test::drawStreamDigest(overlay)) ==
          "351217bcd0222847f528f4b7700b9f5dafa43564fa1b8b104fc80445fa515ced");
    scene.close();
}

} // namespace
