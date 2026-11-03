#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "game/enemies/CombatResources.h"
#include "game/enemies/LevelOpponents.h"
#include "game/enemies/OpponentCombatCheckpoint.h"
#include "game/enemies/ScriptedOpponents.h"
#include "game/hud/HelpMessages.h"
#include "game/level/LevelWorldCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Adapter = OpponentCombatCheckpoint;

std::filesystem::path lootWorld(std::string_view tag, bool general) {
    const auto directory = test::sampleLevel(tag);
    const std::string placement =
        general ? R"(,{"info":2,"position":[0,0,0],"params":[1,0,0,0,0,0,0,0,0,0,0,0]})" : "";
    writeTextFile(directory / "world.json",
                  R"({"objects":[{"name":"FLOOR","position":[0,0,0],"flags":4}],
        "itemInfos":[
          {"type":1,"subtype":1,"name":"COIN","value":500,"radius":1,"height":1},
          {"type":1,"subtype":16,"name":"GARGEAGL","value":1,"radius":1,"height":1},
          {"type":4,"name":"GENERAL","radius":2,"height":6}],
        "itemInstances":[{"info":0,"position":[0,0,0]})" +
                      placement + "]}");
    writeTextFile(directory / "collision.json", R"({"objects":[
      {"object":0,"normals":[0,1,0,0,1,0],
       "vertices":[-100,0,-100,100,0,100,100,0,-100,-100,0,-100,-100,0,100,100,0,100]}]})");
    writeTextFile(directory / "animations.json", R"({"trees":[
      {"name":"COIN","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30}]},
      {"name":"GARGEAGL","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
       "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30}]}]})");
    test::convertModelFixture(directory);
    return directory;
}

struct Fixture {
    struct State {
        Adapter::State adapter;
        EffectTrees::PoolCheckpoint effects;
        std::unique_ptr<LevelWorldCheckpoint::State> world;
    };
    std::filesystem::path root =
        test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    LevelOpponents opponents;
    CombatResources resources;
    std::unique_ptr<Adapter> adapter;
    std::unique_ptr<LevelWorldCheckpoint> worldOwner;
    test::OpponentScript events;
    test::ScriptedOpponents opponentHost{events};
    std::vector<s32> lessons;
    usize awards = 0;
    s32 actor = -1;
    Fixture(std::string_view tag, bool carried = true, bool artwork = true) {
        REQUIRE(world.load(device, lootWorld(tag, carried), {.name = "GLOOT", .directory = "."}));
        world.setPlayerCount(1);
        if (artwork) {
            REQUIRE(weapons.load(root / "WEAPONS"));
        }
        players[0].actor.spawn(3, {}, nullptr, {80, 0, 80}, 0);
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        if (!carried) {
            const auto spawned = opponents.critters().spawnGargoyle({}, 0, "EAGL");
            REQUIRE(spawned);
            actor = *spawned;
        } else {
            for (s32 id = 0; id < Critters::kMost; ++id) {
                if (opponents.critters().alive(id)) {
                    REQUIRE(actor == -1);
                    actor = id;
                }
            }
            REQUIRE(actor >= 0);
            REQUIRE(world.placedItems().size() == 1);
            REQUIRE(world.placedItems().item(0).carried);
        }
        std::vector<CombatResources::Bank> banks;
        if (artwork) {
            banks.push_back({1, &weapons, {}});
        }
        REQUIRE(resources.bind(device, banks));
        adapter = std::make_unique<Adapter>(opponents, resources.resources().sharedEffects);
        worldOwner = std::make_unique<LevelWorldCheckpoint>(world);
        REQUIRE(worldOwner->bindResources(device));
        events.settleBlasts = [] {};
        events.advanceLegend = [](f32) {};
        events.advanceVictory = [](s32, f32) {};
        events.levels = [] {};
        events.award = [this](s32, s32, bool) { ++awards; };
        events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {
            FAIL("the remote party must not be struck by the defeated loot owner");
        };
        events.help = [this](s32 lesson, usize player) {
            CHECK(player == 0); // party index, not input identity 3
            lessons.push_back(lesson);
            return true;
        };
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        adapter.reset();
        worldOwner.reset();
        opponents.close();
        effects.clear();
        resources.clear();
    }
    std::span<const EffectTrees::PoolResource> effectResources() const {
        return resources.resources().sharedEffects;
    }
    std::unique_ptr<State> capture() const {
        auto saved = std::make_unique<State>();
        auto visual = effects.checkpointPool(effectResources());
        REQUIRE(visual);
        saved->effects = std::move(*visual);
        auto value = adapter->checkpoint(saved->effects);
        REQUIRE(value);
        saved->adapter = std::move(*value);
        auto stage = worldOwner->checkpoint();
        REQUIRE(stage);
        saved->world = std::make_unique<LevelWorldCheckpoint::State>(std::move(*stage));
        REQUIRE(saved->adapter.pickupReferences(saved->world->pickups.items.size()));
        return saved;
    }
    void check(const State& expected) const {
        const auto actual = capture();
        CHECK(actual->adapter == expected.adapter);
        CHECK(actual->effects == expected.effects);
        CHECK(*actual->world == *expected.world);
    }
    void restore(const State& saved) {
        const auto uploads = device.texturesCreated;
        const auto heard = lessons;
        const auto paid = awards;
        auto a = adapter->prepareRestore(saved.adapter, saved.effects);
        auto e = effects.prepareRestorePool(saved.effects, effectResources());
        auto w = worldOwner->prepareRestore(*saved.world);
        REQUIRE(a);
        REQUIRE(e);
        REQUIRE(w);
        REQUIRE(adapter->canCommit(*a));
        REQUIRE(effects.canCommit(*e));
        REQUIRE(worldOwner->canCommit(*w));
        REQUIRE(adapter->commitRestore(std::move(*a)));
        REQUIRE(effects.commitRestore(std::move(*e)));
        REQUIRE(worldOwner->commitRestore(std::move(*w)));
        CHECK(device.texturesCreated == uploads);
        CHECK(lessons == heard);
        CHECK(awards == paid);
    }
    void defeat(f32 seconds) {
        EnemyHit hit;
        hit.player = players[0].actor.player();
        hit.damage = 1000000;
        opponents.critters().hurt(actor, hit);
        REQUIRE(opponents.critters().dying(actor));
        // Advance the actual authored death to its drop event. Settlement is a
        // separate native phase, making the retired-but-carried boundary visible.
        for (s32 tick = 0; tick < 2400 && opponents.critters().count() != 0; ++tick) {
            opponents.critters().update(seconds > 0.02f ? 2 : 1, seconds, {});
        }
        REQUIRE(opponents.critters().count() == 0);
    }
    void settle() { opponents.settleRewards(players, opponentHost, opponentHost); }
    void step(f32 seconds) {
        world.capturePresentation();
        effects.capturePresentation();
        world.update(seconds);
        effects.update(seconds);
        opponents.update(seconds > 0.02f ? 2 : 1, seconds, players, {}, opponentHost, opponentHost,
                         opponentHost);
    }
};

TEST_CASE("loot checkpoint restores native carried death bag flight landing and release",
          "[opponent-loot-checkpoint][assets]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const bool carried = GENERATE(true, false);
    CAPTURE(seconds, carried);
    auto host = std::make_unique<Fixture>("opponent-loot-host", carried);
    auto guest = std::make_unique<Fixture>("opponent-loot-guest", carried);
    host->defeat(seconds);
    const auto pending = host->capture();
    REQUIRE_FALSE(pending->adapter.critters.events.losses.empty());
    REQUIRE_FALSE(pending->adapter.dormant());
    CHECK(pending->adapter.carried[static_cast<usize>(host->actor)].has_value() == carried);
    // Preparing before native loss settlement must still undo the newly born
    // bag, its consumed cargo index, shared seed and effect handle allocation.
    auto undo = host->adapter->prepareRestore(pending->adapter, pending->effects);
    auto effectsUndo = host->effects.prepareRestorePool(pending->effects, host->effectResources());
    auto worldUndo = host->worldOwner->prepareRestore(*pending->world);
    REQUIRE(undo);
    REQUIRE(effectsUndo);
    REQUIRE(worldUndo);
    host->settle();
    const auto born = host->capture();
    REQUIRE(born->adapter.bags.size() == 1);
    const auto& bag = born->adapter.bags.front();
    CHECK(bag.item.has_value() == carried);
    CHECK(bag.name == (carried ? "" : "GARGEAGL"));
    CHECK(bag.age == 0);
    CHECK(bag.velocity == 20);
    CHECK_FALSE(bag.landed);
    CHECK_FALSE(born->adapter.carried[static_cast<usize>(host->actor)]);
    CHECK(born->adapter.bagSeed == pending->adapter.bagSeed * 1664525U + 1013904223U);
    CHECK(host->lessons == std::vector<s32>{carried ? HelpMessages::kGeneralsCarry
                                                    : HelpMessages::kGargoylesRelease});
    REQUIRE(host->adapter->canCommit(*undo));
    REQUIRE(host->effects.canCommit(*effectsUndo));
    REQUIRE(host->worldOwner->canCommit(*worldUndo));
    const auto uploads = host->device.texturesCreated;
    const auto lessons = host->lessons;
    REQUIRE(host->adapter->commitRestore(std::move(*undo)));
    REQUIRE(host->effects.commitRestore(std::move(*effectsUndo)));
    REQUIRE(host->worldOwner->commitRestore(std::move(*worldUndo)));
    host->check(*pending);
    CHECK(host->lessons == lessons);
    host->lessons.clear();
    host->settle();
    host->check(*born);
    CHECK(host->device.texturesCreated == uploads);
    guest->restore(*born);
    guest->check(*born);
    bool airborne = false;
    bool landed = false;
    bool released = false;
    for (s32 tick = 0; tick < static_cast<s32>(3.5f / seconds); ++tick) {
        CAPTURE(tick);
        if (tick % 11 == 0) {
            guest->restore(*host->capture());
        }
        host->step(seconds);
        guest->step(seconds);
        const auto expected = host->capture();
        guest->check(*expected);
        if (!expected->adapter.bags.empty()) {
            const auto& current = expected->adapter.bags.front();
            airborne |= !current.landed && current.position.y > 1;
            landed |= current.landed;
            CHECK_FALSE(expected->adapter.dormant());
        } else {
            released = true;
        }
    }
    CHECK(airborne);
    CHECK(landed);
    CHECK(released);
    const auto& items = host->world.placedItems();
    REQUIRE(items.size() == (carried ? 1 : 2));
    const auto& reward = items.item(carried ? 0 : 1);
    CHECK_FALSE(reward.carried);
    CHECK(reward.visible);
    CHECK(reward.takeable());
    CHECK(reward.name == (carried ? "COIN" : "GARGEAGL"));
}

TEST_CASE("loot checkpoint retains expired bag handles until native cleanup",
          "[opponent-loot-checkpoint][assets]") {
    auto fixture = std::make_unique<Fixture>("opponent-loot-expired");
    fixture->defeat(1.0f / 60);
    fixture->settle();
    auto born = fixture->capture();
    REQUIRE(born->adapter.bags.size() == 1);
    fixture->effects.stop(born->adapter.bags.front().effect);
    const auto expired = fixture->capture();
    REQUIRE(expired->effects.effects.empty());
    REQUIRE(expired->adapter.bags.size() == 1);
    REQUIRE(expired->adapter.references(1U << 3, expired->effects));
    fixture->step(1.0f / 60);
    REQUIRE(fixture->capture()->adapter.bags.empty());
    CHECK_FALSE(fixture->world.placedItems().item(0).carried);
    fixture->restore(*expired);
    CHECK(fixture->world.placedItems().item(0).carried);
    fixture->step(1.0f / 60);
    CHECK(fixture->capture()->adapter.bags.empty());
    CHECK_FALSE(fixture->world.placedItems().item(0).carried);
}

TEST_CASE("missing native bag artwork releases the reward without losing undo state",
          "[opponent-loot-checkpoint][assets]") {
    auto fixture = std::make_unique<Fixture>("opponent-loot-no-art", true, false);
    fixture->defeat(1.0f / 30);
    const auto before = fixture->capture();
    fixture->settle();
    const auto released = fixture->capture();
    CHECK(released->adapter.bags.empty());
    CHECK_FALSE(released->adapter.carried[static_cast<usize>(fixture->actor)]);
    CHECK_FALSE(fixture->world.placedItems().item(0).carried);
    CHECK(released->adapter.bagSeed != before->adapter.bagSeed);
    fixture->restore(*before);
    fixture->settle();
    fixture->check(*released);
}

TEST_CASE("loot checkpoint rejects malformed references before any native owner changes",
          "[opponent-loot-checkpoint][assets]") {
    auto fixture = std::make_unique<Fixture>("opponent-loot-invalid");
    fixture->defeat(1.0f / 30);
    fixture->settle();
    const auto baseline = fixture->capture();
    for (s32 variant = 0; variant < 10; ++variant) {
        CAPTURE(variant);
        auto invalid = baseline->adapter;
        auto& bag = invalid.bags.front();
        switch (variant) {
        case 0: bag.position.x = std::numeric_limits<f32>::quiet_NaN(); break;
        case 1: bag.age = -0.1f; break;
        case 2: bag.age = 3; break;
        case 3: bag.effect = baseline->effects.nextId; break;
        case 4: bag.effect = 0; break;
        case 5: bag.name = "COIN"; break;
        case 6: bag.item.reset(); break;
        case 7: bag.velocity = std::numeric_limits<f32>::infinity(); break;
        case 8: bag.spin = -1; break;
        case 9: invalid.carried.back() = Adapter::State::kMaxRecords + 1; break;
        default: break;
        }
        CHECK_FALSE(fixture->adapter->prepareRestore(invalid, baseline->effects));
        fixture->check(*baseline);
    }
    auto unlinked = baseline->adapter;
    unlinked.bags.front().item = baseline->world->pickups.items.size();
    REQUIRE(unlinked.valid()); // bounds require the incoming world's exact list
    CHECK_FALSE(unlinked.pickupReferences(baseline->world->pickups.items.size()));
    CHECK(unlinked.pickupReferences(baseline->world->pickups.items.size() + 1));
    unlinked.carried.back() = 77;
    CHECK_FALSE(unlinked.pickupReferences(77));
    CHECK(unlinked.pickupReferences(78));
    CHECK_FALSE(unlinked.dormant());
    auto first = fixture->adapter->prepareRestore(baseline->adapter, baseline->effects);
    auto sibling = fixture->adapter->prepareRestore(baseline->adapter, baseline->effects);
    REQUIRE(first);
    REQUIRE(sibling);
    const Adapter foreign{fixture->opponents, fixture->effectResources()};
    CHECK_FALSE(foreign.canCommit(*first));
    REQUIRE(fixture->adapter->commitRestore(std::move(*first)));
    CHECK_FALSE(fixture->adapter->canCommit(*sibling));
    auto rebound = fixture->adapter->prepareRestore(baseline->adapter, baseline->effects);
    REQUIRE(rebound);
    fixture->opponents.close();
    CHECK_FALSE(fixture->adapter->canCommit(*rebound));
}

TEST_CASE("capturing cargo never grants dormant replay admission", "[opponent-loot-checkpoint]") {
    Adapter::State state;
    REQUIRE(state.valid());
    REQUIRE(state.dormant());
    state.carried.back() = 0;
    REQUIRE(state.valid());
    CHECK_FALSE(state.pickupReferences(0));
    CHECK(state.pickupReferences(1));
    CHECK_FALSE(state.dormant());
    state.carried.back().reset();
    state.bags.push_back({std::nullopt, "GARGEAGL", {}, 20, 0, 0, 0.5f, 1, false});
    REQUIRE(state.valid());
    CHECK(state.pickupReferences(0)); // named births have no existing pickup index
    CHECK_FALSE(state.dormant());
    EffectTrees::PoolCheckpoint effects;
    CHECK_FALSE(state.references(0, effects));
    effects.nextId = 2;
    CHECK(state.references(0, effects));
}
} // namespace
