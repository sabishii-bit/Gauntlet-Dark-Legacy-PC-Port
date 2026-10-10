#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PlayReplication.h"
#include "game/screens/ReplicaStage.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("every playable native stage loads an independent four-player presentation catalog",
          "[netplay][replica-stage][assets]") {
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const GameConfig config;
    GameContext game;
    game.config = &config;
    game.levels = &catalog;
    game.unpackedRoot = root;
    std::vector<PartyMember> party;
    PartyBootstrap::Party profiles;
    for (s32 seat = 0; seat < 4; ++seat) {
        CharacterSave save;
        save.name = "TEST";
        save.character = seat;
        save.color = seat;
        save.progress().experience = levelExperience(80);
        save.progress().health = 9000;
        profiles[static_cast<usize>(seat)] = CharacterProfile::capture(save);
        REQUIRE(profiles[static_cast<usize>(seat)]);
        party.push_back({seat, save});
    }
    std::vector<LevelRef> levels{LevelRef::tower()};
    for (const auto& realm : catalog.realms()) {
        // L2 and later are auxiliary tower scenes; PlayScene explicitly reloads
        // L1 for tower gameplay. They are not selectable gameplay destinations.
        if (realm.id == LevelRef::kTowerRealm) {
            continue;
        }
        for (const auto& name : realm.levels) {
            REQUIRE(catalog.byName(name));
            levels.push_back(*catalog.byName(name));
        }
    }
    usize count = 0;
    MatchSession match;
    REQUIRE(match.open(1, {1, 1, 1, 1}, {}));
    ReplicaStage remote;
    for (const auto& level : levels) {
        CAPTURE(level.name);
        LevelWorld world;
        REQUIRE(world.load(device, root, level));
        PlayScene scene;
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, game, world, party, options));
        ProjectileResources projectiles;
        PickupResources pickups;
        FixtureResources fixtures;
        REQUIRE(scene.bindReplicationResources(projectiles, pickups, fixtures));
        const auto sceneId = static_cast<u32>(count + 1);
        REQUIRE(match.prepare(sceneId,
                              count == 0 ? MatchTransition::Start : MatchTransition::Travel,
                              count == 0 ? nullptr : &profiles));
        PlayReplication driver;
        REQUIRE(driver.bind(scene, match, pickups, fixtures));
        REQUIRE(remote.open(device, root, level, match.context(), profiles));
        REQUIRE(remote.world() != &world);
        REQUIRE(remote.view());
        REQUIRE_FALSE(remote.view()->shown());
        REQUIRE(match.loaded());
        REQUIRE(match.phase() == MatchSession::Phase::Running);
        for (s32 tick = 0; tick < 7; ++tick) {
            REQUIRE(driver.advance(match, {}, projectiles) == PlayReplication::Result::Advanced);
            REQUIRE(driver.latest());
            INFO(remote.view()->rejection(*driver.latest()));
            REQUIRE(remote.show(*driver.latest()));
        }
        const auto before = device.texturesCreated;
        device.draws.clear();
        remote.draw(device, Mat4{1}, 1280, 720, 0);
        CHECK_FALSE(device.draws.empty());
        CHECK(device.texturesCreated == before);
        // Failed and stale loads cannot destroy a scene or its borrowed catalogs.
        const auto* loaded = remote.world();
        auto missing = profiles;
        missing[3].reset();
        auto next = match.context();
        ++next.epoch;
        CHECK_FALSE(remote.open(device, root, level, next, missing));
        CHECK_FALSE(remote.open(device, root, level, match.context(), profiles));
        if (count == 0) {
            next.transition = MatchTransition::Travel;
            CHECK_FALSE(
                remote.open(device, root / "unavailable-stage-root", level, next, profiles));
            auto auxiliary = level;
            auxiliary.name = "L2";
            CHECK_FALSE(remote.open(device, root, auxiliary, next, profiles));
        }
        CHECK(remote.world() == loaded);
        CHECK(remote.show(*driver.latest()));
        const auto oldState = *driver.latest();
        REQUIRE(match.requestPause());
        REQUIRE(match.prepare(sceneId, MatchTransition::Resume));
        REQUIRE(remote.resume(match.context()));
        CHECK(remote.view()->shown()->motion.epoch == oldState.motion.epoch);
        CHECK_FALSE(remote.show(oldState));
        REQUIRE(driver.bind(scene, match, pickups, fixtures));
        REQUIRE(match.loaded());
        REQUIRE(driver.advance(match, {}, projectiles) == PlayReplication::Result::Advanced);
        REQUIRE(driver.latest());
        REQUIRE(remote.show(*driver.latest()));
        CHECK(remote.world() == loaded);
        CHECK(device.texturesCreated == before);
        ++count;
    }
    CHECK(count > 50);
}
} // namespace
