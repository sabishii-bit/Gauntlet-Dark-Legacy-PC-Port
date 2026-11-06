#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/effects/EffectObservation.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/LevelOpponents.h"
#include "game/enemies/ScriptedOpponents.h"
#include "game/hud/HelpMessages.h"
#include "game/level/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

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

/** One effect as two separately loaded pools can compare it: its handle and its values. */
struct EffectView {
    u32 id = 0;
    std::optional<test::EffectObservation> state;
    bool operator==(const EffectView&) const = default;
};

/** One pickup as two separately loaded worlds can compare it. */
struct PickupView {
    std::string name;
    s32 value = 0;
    Vec3 position{0};
    Vec3 velocity{0};
    bool visible = false;
    bool taken = false;
    bool thrown = false;
    bool carried = false;
    f32 noGrabSeconds = 0;
    bool operator==(const PickupView&) const = default;
};

struct Fixture {
    /** A local rewind point of the owners the loot touches. */
    struct Saved {
        LevelOpponents::LocalSnapshot opponents;
        EffectTrees::LocalSnapshot effects;
        LevelWorld::LocalSnapshot world;
        std::vector<EffectView> visual; ///< the effects then, which have no local equality
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
        opponents.close();
        effects.clear();
    }
    std::vector<EffectView> visual() const {
        std::vector<EffectView> result;
        for (usize index = 0; index < effects.count(); ++index) {
            const u32 id = effects.effect(index).id;
            result.push_back({id, test::observeEffect(effects, id)});
        }
        return result;
    }
    std::vector<PickupView> pickups() const {
        const auto& items = world.placedItems();
        std::vector<PickupView> result;
        for (usize index = 0; index < items.size(); ++index) {
            const auto& item = items.item(index);
            result.push_back({item.name, item.value, item.position, item.velocity, item.visible,
                              item.taken, item.thrown, item.carried, item.noGrabSeconds});
        }
        return result;
    }
    std::vector<std::optional<usize>> cargo() const {
        std::vector<std::optional<usize>> result;
        result.reserve(static_cast<usize>(Critters::kMost));
        for (s32 slot = 0; slot < Critters::kMost; ++slot) {
            result.push_back(opponents.carriedBy(slot));
        }
        return result;
    }
    std::vector<LevelOpponents::Bag> bags() const {
        const auto thrown = opponents.thrownBags();
        return {thrown.begin(), thrown.end()};
    }
    std::unique_ptr<Saved> save() const {
        auto saved = std::make_unique<Saved>();
        REQUIRE(opponents.saveLocal(saved->opponents));
        effects.saveLocal(saved->effects);
        world.saveLocal(saved->world);
        saved->visual = visual();
        return saved;
    }
    void check(const Saved& expected) const {
        const auto actual = save();
        CHECK(opponents.equalLocal(expected.opponents, actual->opponents));
        CHECK(actual->visual == expected.visual);
        CHECK(world.equalLocal(expected.world, actual->world));
    }
    /** Whether `other`, loaded apart from this, holds the same loot: the bags, the cargo,
     * the shared seed, the effects and the pickups. */
    void compare(const Fixture& other) const {
        CHECK(bags() == other.bags());
        CHECK(cargo() == other.cargo());
        CHECK(opponents.bagSpinSeed() == other.opponents.bagSpinSeed());
        CHECK(visual() == other.visual());
        CHECK(pickups() == other.pickups());
    }
    void restore(const Saved& saved) {
        const auto uploads = device.texturesCreated;
        const auto heard = lessons;
        const auto paid = awards;
        REQUIRE(opponents.canRestoreLocal(saved.opponents));
        REQUIRE(effects.canRestoreLocal(saved.effects));
        REQUIRE(world.canRestoreLocal(saved.world));
        REQUIRE(opponents.restoreLocal(saved.opponents));
        REQUIRE(effects.restoreLocal(saved.effects));
        REQUIRE(world.restoreLocal(saved.world));
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
    guest->defeat(seconds);
    // Rewinding to before native loss settlement must still undo the newly born
    // bag, its consumed cargo index, shared seed and effect handle allocation.
    const auto pending = host->save();
    CHECK(host->opponents.carriedBy(host->actor).has_value() == carried);
    const u32 seed = host->opponents.bagSpinSeed();
    host->settle();
    const auto born = host->save();
    REQUIRE(host->opponents.thrownBags().size() == 1);
    const auto& bag = host->opponents.thrownBags().front();
    CHECK(bag.item.has_value() == carried);
    CHECK(bag.name == (carried ? "" : "GARGEAGL"));
    CHECK(bag.age == 0);
    CHECK(bag.velocity == 20);
    CHECK_FALSE(bag.landed);
    CHECK_FALSE(host->opponents.carriedBy(host->actor));
    CHECK(host->opponents.bagSpinSeed() == seed * 1664525U + 1013904223U);
    CHECK(host->lessons == std::vector<s32>{carried ? HelpMessages::kGeneralsCarry
                                                    : HelpMessages::kGargoylesRelease});
    const auto uploads = host->device.texturesCreated;
    const auto lessons = host->lessons;
    host->restore(*pending);
    host->check(*pending);
    CHECK(host->lessons == lessons);
    host->lessons.clear();
    host->settle();
    host->check(*born);
    CHECK(host->device.texturesCreated == uploads);
    guest->settle();
    host->compare(*guest);
    bool airborne = false;
    bool landed = false;
    bool released = false;
    for (s32 tick = 0; tick < static_cast<s32>(3.5f / seconds); ++tick) {
        CAPTURE(tick);
        if (tick % 11 == 0) {
            // The host runs two steps ahead and takes them back: a bag in flight must
            // have moved on, and nothing of the abandoned steps may be left.
            const auto saved = host->save();
            const bool inFlight = !host->opponents.thrownBags().empty();
            host->step(seconds);
            host->step(seconds);
            if (inFlight) {
                const auto abandoned = host->save();
                CHECK_FALSE(host->opponents.equalLocal(saved->opponents, abandoned->opponents));
            }
            host->restore(*saved);
            host->check(*saved);
        }
        host->step(seconds);
        guest->step(seconds);
        host->compare(*guest);
        const auto thrown = host->opponents.thrownBags();
        if (!thrown.empty()) {
            const auto& current = thrown.front();
            airborne |= !current.landed && current.position.y > 1;
            landed |= current.landed;
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
    REQUIRE(fixture->opponents.thrownBags().size() == 1);
    fixture->effects.stop(fixture->opponents.thrownBags().front().effect);
    const auto expired = fixture->save();
    REQUIRE(fixture->effects.count() == 0);
    REQUIRE(fixture->opponents.thrownBags().size() == 1);
    fixture->step(1.0f / 60);
    REQUIRE(fixture->opponents.thrownBags().empty());
    CHECK_FALSE(fixture->world.placedItems().item(0).carried);
    fixture->restore(*expired);
    CHECK(fixture->world.placedItems().item(0).carried);
    fixture->step(1.0f / 60);
    CHECK(fixture->opponents.thrownBags().empty());
    CHECK_FALSE(fixture->world.placedItems().item(0).carried);
}

TEST_CASE("missing native bag artwork releases the reward without losing undo state",
          "[opponent-loot-checkpoint][assets]") {
    auto fixture = std::make_unique<Fixture>("opponent-loot-no-art", true, false);
    fixture->defeat(1.0f / 30);
    const auto before = fixture->save();
    const u32 seed = fixture->opponents.bagSpinSeed();
    fixture->settle();
    const auto released = fixture->save();
    CHECK(fixture->opponents.thrownBags().empty());
    CHECK_FALSE(fixture->opponents.carriedBy(fixture->actor));
    CHECK_FALSE(fixture->world.placedItems().item(0).carried);
    CHECK(fixture->opponents.bagSpinSeed() != seed);
    fixture->restore(*before);
    fixture->settle();
    fixture->check(*released);
}
} // namespace
