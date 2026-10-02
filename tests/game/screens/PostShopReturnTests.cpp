#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/Scenario.h"
#include "game/config/GameConfig.h"
#include "game/players/CharacterSave.h"
#include "game/screens/AfterLevelScene.h"
#include "game/screens/PlayScene.h"
#include "game/screens/PlayerSelectScene.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("completed shop saves its character before returning to the province wing",
          "[post-shop-return][shop][select][assets]") {
    // game_main's do_shop result 2 enters init_player_select(2), then resumes
    // at the realm's SetPlayerStartPos marker rather than the tower entrance.
    const auto root = test::assetOrSkip("SHPDATA/SHOP.WAD").parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2");
    const auto scenario = Scenario::load(test::dataDirectory().parent_path() /
                                         "tests/scenarios/after-level-return-province.json");
    const auto initial = scenario.partyMembers();
    REQUIRE(initial.size() == 1);
    REQUIRE_FALSE(initial.front().slot.has_value());
    test::FakeRenderDevice device;
    GameConfig config;
    config.save.directory = test::scratchDirectory("post-shop-return-save").string();
    config.save.slots = 4;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    LevelWorld world;
    REQUIRE(world.load(device, root));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.unpackedRoot = root;
    context.tower = &world;
    AfterLevelScene shop;
    REQUIRE(shop.open(device, context, initial, scenario.results, {1000, 100, 1000}, "G1"));
    bool visitedShop = false;
    bool visitedInventory = false;
    bool finished = false;
    for (s32 press = 0; press < 40 && !finished; ++press) {
        const auto phase = shop.session().lanes().front().phase;
        visitedShop |= phase == ShopPhase::Shopping;
        visitedInventory |= phase == ShopPhase::Inventory;
        ShopSession::Inputs input{};
        input[0].select = true;
        finished = shop.update(1.0 / 60, input);
        finished = shop.update(0.5, {}) || finished;
    }
    REQUIRE(finished);
    REQUIRE(visitedShop);
    REQUIRE(visitedInventory);
    const auto afterShop = shop.session().party();
    REQUIRE(afterShop.front().save.toJson() == initial.front().save.toJson());
    shop.close();

    PlayerSelectScene select;
    REQUIRE(select.openAfterLevel(device, context, afterShop));
    REQUIRE(select.lane(0).state() == SelectLane::State::SaveMenu);
    REQUIRE(select.step(60, {}) == SelectOutcome::Running);
    const auto input = [&](const MenuInput& buttons) {
        PlayerSelectScene::Inputs inputs{};
        inputs[0] = buttons;
        return select.step(1, inputs);
    };
    MenuInput down;
    down.down = true;
    input(down); // Done wraps to Save.
    input({});
    MenuInput accept;
    accept.select = true;
    input(accept);
    REQUIRE(select.lane(0).state() == SelectLane::State::SavePick);
    input({});
    input(accept);
    REQUIRE(select.lane(0).state() == SelectLane::State::Saving);
    for (s32 tick = 0; tick < 180; ++tick) {
        select.step(1, {});
    }
    REQUIRE(select.lane(0).state() == SelectLane::State::SaveMenu);
    REQUIRE(select.lane(0).slotInUse() == 0);
    CharacterSave stored;
    REQUIRE(select.saves().load(0, stored));
    CHECK(stored.toJson() == afterShop.front().save.toJson());
    input(accept); // Done after the save notice.
    REQUIRE(select.lane(0).lockedIn());
    SelectOutcome result = SelectOutcome::Running;
    for (s32 tick = 0; tick < 180 && result == SelectOutcome::Running; ++tick) {
        result = select.step(1, {});
    }
    REQUIRE(result == SelectOutcome::Done);
    const auto returning = select.party();
    REQUIRE(returning.size() == 1);
    CHECK(returning.front().slot == 0);
    CHECK(returning.front().save.toJson() == stored.toJson());
    select.close();

    PlayOptions options;
    options.welcome = false;
    options.arriving = true;
    options.arrivalWorld = 7;
    PlayScene play;
    REQUIRE(play.open(device, context, world, returning, options));
    REQUIRE(play.actor(0) != nullptr);
    const auto* arrival = world.arrivalPoint(7);
    REQUIRE(arrival != nullptr);
    CHECK(glm::distance(play.actor(0)->position(), arrival->position) < 4);
    CHECK(glm::distance(play.actor(0)->position(), world.startPoint(0)->position) > 10);
    REQUIRE(play.startCamera().active());
    const auto from = play.viewCamera().position;
    for (s32 frame = 0; frame < 180; ++frame) {
        REQUIRE(play.update(1.0 / 30, {}) == PlayOutcome::Running);
    }
    CHECK_FALSE(play.startCamera().active());
    CHECK(play.viewCamera().position.y < from.y);
    CHECK(play.party().front().slot == 0);
    play.close();
}
} // namespace
