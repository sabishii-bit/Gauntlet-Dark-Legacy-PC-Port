#include <array>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("scene damage vibrates the actual input player and pause or close stops motors",
          "[game][screens][assets][rumble]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const GameConfig config;
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *levels.byName("G1")));
    GameContext context;
    context.config = &config;
    context.tower = &world;
    context.levels = &levels;
    context.unpackedRoot = root;
    std::vector<std::pair<s32, s32>> vibrations;
    s32 stops = 0;
    context.vibrate = [&](s32 player, s32 frames) { vibrations.emplace_back(player, frames); };
    context.stopVibration = [&] { ++stops; };
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{10.7f, 10.2f, -60.5f};
    CharacterSave save;
    save.progress().health = 2000;
    const std::array party{PartyMember{3, save}, PartyMember{1, save}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    const auto entering = [&] {
        return scene.spawning() || scene.animator(3)->entering() || scene.animator(1)->entering();
    };
    for (s32 i = 0; i < 400 && entering(); ++i) {
        scene.update(1.0 / 60.0, {});
    }
    REQUIRE_FALSE(entering());
    vibrations.clear();
    scene.harm(1, 5, HurtKind::Blow);
    REQUIRE(vibrations.size() == 1);
    CHECK(vibrations.back() == std::pair<s32, s32>{1, 10});
    scene.harm(3, 5, HurtKind::Blow);
    REQUIRE(vibrations.size() == 2);
    CHECK(vibrations.back() == std::pair<s32, s32>{3, 10});
    const s32 before = stops;
    scene.pauseGameplaySounds();
    CHECK(stops == before + 1);
    scene.close();
    CHECK(stops == before + 2);
    scene.close(); // callbacks no longer borrow a potentially destroyed application
    CHECK(stops == before + 2);
}
} // namespace
