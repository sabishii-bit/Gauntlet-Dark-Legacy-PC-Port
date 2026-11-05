#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/critters/CritterStatues.h"
#include "game/critters/Critters.h"
#include "game/enemies/LevelOpponents.h"
#include "game/enemies/ScriptedOpponents.h"
#include "game/hud/HelpMessages.h"
#include "game/level/NativeStateResources.h"
#include "game/party/AttackSceneFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A level's opponents with one grunt generator, and one player standing away from it. */
struct OpponentsFixture {
    std::filesystem::path root = test::attackSceneAssets();
    test::FakeRenderDevice device;
    LevelWorld world;
    WorldLayout layout;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    LevelOpponents opponents;

    OpponentsFixture() {
        players[0].actor.spawn(2, {}, nullptr, {0, 0, 20}, 0);
        opponents.open({device, world, weapons, effects, audio, root, 1}, players);
        opponents.enemies().open(device, root, nullptr, 2, {.health = 1000}, 1);
        REQUIRE(opponents.enemies().loadKind(kGruntKind));
        writeTextFile(root / "world.json", R"({
          "objects":[{"name":"GROUND","position":[0,0,0]}],
          "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
          "itemInstances":[{"info":0,"minPlayers":1,"position":[8,0,0],
                            "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})");
        REQUIRE(layout.load(root));
        REQUIRE(opponents.generators().bind(device, layout, opponents.enemies(), nullptr, {}, 1));
        REQUIRE(opponents.generators().count() == 1);
    }
    OpponentsFixture(const OpponentsFixture&) = delete;
    OpponentsFixture& operator=(const OpponentsFixture&) = delete;
    ~OpponentsFixture() { opponents.close(); }
};

/** The opponents' own retained values (rewards, lessons, bags and the rest), which hash
 * without borrowed resources, so two separately loaded owners can be compared. */
NativeStateDigest::Digest followupDigest(const LevelOpponents& opponents) {
    NativeStateDigest digest;
    REQUIRE(opponents.appendNativeFollowupDigest(digest));
    const auto result = digest.checkedFinish();
    REQUIRE(result);
    return *result;
}

constexpr std::string_view kSwarmPrefix = "stage/opponents-local-rewind/swarm";

/** The native digest of one of the swarm's owners, its artwork named alike in every load;
 * none when that owner has nothing to hash (a closed pool). */
template <class Owner>
std::optional<NativeStateDigest::Digest> swarmDigest(const LevelOpponents& opponents,
                                                     const Owner& owner) {
    NativeStateResources resources;
    if (!opponents.enemies().registerNativeResources(resources, kSwarmPrefix) ||
        !opponents.generators().registerNativeResources(resources)) {
        return std::nullopt;
    }
    NativeStateDigest digest(resources.resources());
    if (!owner.appendNativeDigest(digest)) {
        return std::nullopt;
    }
    return digest.checkedFinish();
}

TEST_CASE("native hit streak and generator rewards replay in their original settlement order",
          "[level-opponents-local-rewind][generator-lesson]") {
    OpponentsFixture fixture;
    const auto enemy = fixture.opponents.enemies().spawn(
        EnemySpawn{.kind = kGruntKind, .tier = 1, .generator = 0, .placed = true}, {});
    REQUIRE(enemy);
    for (s32 hit = 0; hit < 9; ++hit) {
        fixture.opponents.strikeEnemy(*enemy, 5, 0, {0, 0, 1}, 2, fixture.players);
    }
    REQUIRE(fixture.opponents.hitStreakOf(2) == 9);
    REQUIRE(fixture.opponents.pendingLessons().empty());
    fixture.opponents.strikeEnemy(*enemy, 5, 0, {0, 0, 1}, 2, fixture.players);
    fixture.opponents.strikeGenerator(0, 1, 1, fixture.players);
    fixture.opponents.strikeGenerator(0, 100000, 2, fixture.players);
    const auto lessons = fixture.opponents.pendingLessons();
    REQUIRE(std::vector(lessons.begin(), lessons.end()) ==
            std::vector<std::pair<s32, s32>>{{HelpMessages::kDestroyGenerators, 2}});
    const auto rewards = fixture.opponents.pendingGeneratorRewards();
    REQUIRE(rewards.size() == 2);
    REQUIRE_FALSE(rewards[0].destroyed);
    REQUIRE(rewards[1].destroyed);
    CHECK(fixture.opponents.hitStreakOf(2) == 0); // destruction resets only its hitter's streak
    auto queued = std::make_unique<LevelOpponents::LocalSnapshot>();
    REQUIRE(fixture.opponents.saveLocal(*queued));
    std::vector<std::string> events;
    test::OpponentScript callbacks;
    test::ScriptedOpponents opponentHost{callbacks};
    callbacks.help = [&](s32 lesson, usize player) {
        CHECK(lesson == HelpMessages::kDestroyGenerators);
        CHECK(player == 0); // identity 2 is the sole party entry
        events.emplace_back("lesson");
        return true;
    };
    callbacks.award = [&](s32 player, s32 amount, bool killed) {
        events.push_back(std::to_string(player) + ":" + std::to_string(amount) +
                         (killed ? ":kill" : ":hit"));
    };
    callbacks.destroyedGenerator = [&](s32 player) {
        events.push_back("generator:" + std::to_string(player));
    };
    callbacks.levels = [&] { events.emplace_back("levels"); };
    fixture.opponents.settleRewards(fixture.players, opponentHost, opponentHost);
    REQUIRE(events.size() >= 5);
    CHECK(events.front() == "lesson");
    CHECK(events[events.size() - 2] == "generator:2");
    CHECK(events.back() == "levels");
    const auto expected = events;
    events.clear();
    REQUIRE(fixture.opponents.restoreLocal(*queued));
    CHECK(events.empty());
    fixture.opponents.settleRewards(fixture.players, opponentHost, opponentHost);
    CHECK(events == expected);
    events.clear();
    fixture.opponents.settleRewards(fixture.players, opponentHost, opponentHost);
    CHECK(events == std::vector<std::string>{"levels"});
}

TEST_CASE("dormant statue admission uses the native grid wake and body predicates without mutation",
          "[dormant-opponents][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    Critters resources;
    resources.open(device, root, nullptr, {}, 'G');
    auto* archive = resources.archiveFor(CombatantKind::Golem);
    REQUIRE(archive);
    CritterStatues statues;
    CritterStatues::Placement placement;
    placement.kind = CombatantKind::Golem;
    placement.radius = 4;
    placement.height = 5;
    placement.viewRadius = 10;
    placement.sight = 10;
    CritterStatues::Visitor visitor{{0, 0, 12}, 1, 5};
    const s32 variant = GENERATE(0, 1, 2, 3, 4);
    switch (variant) {
    case 1: visitor.position.z = 10; break;
    case 2: visitor.position = {0, 20, 0}; break;
    case 3:
        placement.sight = -1;
        visitor.position.z = 4.5f;
        break;
    case 4:
        placement.sight = 100;
        visitor.position.z = 90;
        statues.setContactGridBounds({-100, 0, -100}, {100, 0, 100});
        break;
    default: break;
    }
    REQUIRE(statues.add(device, *archive, placement, nullptr));
    CritterStatues::LocalSnapshot before;
    statues.saveLocal(before);
    const bool quiet = statues.dormantFor(std::span{&visitor, 1});
    CritterStatues::LocalSnapshot asked;
    statues.saveLocal(asked);
    CHECK(statues.equalLocal(before, asked));
    const Vec3 after = statues.touch(visitor.position, visitor.radius, visitor.height);
    const bool changed = statues.woken(0) || after != visitor.position;
    CHECK(quiet == !changed);
    if (quiet) {
        statues.update(1, 1.0f / 60, [](const Vec3&, f32) { return true; });
        CritterStatues::LocalSnapshot updated;
        statues.saveLocal(updated);
        CHECK(statues.equalLocal(before, updated));
    } else {
        CHECK_FALSE(statues.dormantFor(std::span{&visitor, 1}));
    }
}

TEST_CASE("opponent rewind preserves defeated critters and their pending rewards without dispatch",
          "[level-opponents-local-rewind][assets]") {
    const auto native = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    OpponentsFixture fixture;
    auto& critters = fixture.opponents.critters();
    critters.open(fixture.device, native, nullptr, {}, 'G');
    const auto general = critters.spawnGeneral({}, 0);
    REQUIRE(general);
    auto alive = std::make_unique<LevelOpponents::LocalSnapshot>();
    REQUIRE(fixture.opponents.saveLocal(*alive));
    EnemyHit hit;
    hit.damage = 1000000;
    hit.player = 2;
    critters.hurt(*general, hit);
    REQUIRE(critters.dying(*general));
    for (s32 frame = 0; frame < 1200 && critters.count() != 0; ++frame) {
        critters.update(2, 1.0f / 30, {});
    }
    REQUIRE(critters.count() == 0);
    auto dead = std::make_unique<LevelOpponents::LocalSnapshot>();
    REQUIRE(fixture.opponents.saveLocal(*dead));
    CHECK_FALSE(fixture.opponents.equalLocal(*alive, *dead));
    REQUIRE(fixture.opponents.restoreLocal(*alive));
    auto restored = std::make_unique<LevelOpponents::LocalSnapshot>();
    REQUIRE(fixture.opponents.saveLocal(*restored));
    CHECK(fixture.opponents.equalLocal(*alive, *restored));
    CHECK(critters.alive(*general));
    REQUIRE(fixture.opponents.restoreLocal(*dead));
    CHECK_FALSE(critters.alive(*general));
    CHECK_FALSE(critters.takeLosses().empty()); // Saving and restoring never dispatch rewards.
}

TEST_CASE("native opponent owner replays wrong branches through pools queues and rewards",
          "[level-opponents-local-rewind][native-equality]") {
    auto reference = std::make_unique<OpponentsFixture>();
    auto predicted = std::make_unique<OpponentsFixture>();
    const auto prepare = [](OpponentsFixture& f) {
        const auto enemy = f.opponents.enemies().spawn(
            EnemySpawn{.kind = kGruntKind, .tier = 1, .generator = 0, .placed = true}, {});
        REQUIRE(enemy);
        for (s32 hit = 0; hit < 10; ++hit) {
            f.opponents.strikeEnemy(*enemy, 1, 0, {0, 0, 1}, 2, f.players);
        }
        f.opponents.strikeGenerator(0, 1, 2, f.players);
    };
    prepare(*reference);
    prepare(*predicted);
    REQUIRE_FALSE(predicted->opponents.pendingLessons().empty());
    REQUIRE_FALSE(predicted->opponents.pendingGeneratorRewards().empty());
    auto saved = std::make_unique<LevelOpponents::LocalSnapshot>();
    std::vector<LevelOpponents::LocalSnapshot> expected(8);
    auto actual = std::make_unique<LevelOpponents::LocalSnapshot>();
    EffectTrees::LocalSnapshot effects;
    usize callbacks = 0;
    test::OpponentScript events;
    test::ScriptedOpponents opponentHost{events};
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [&] { ++callbacks; };
    events.award = [&](s32, s32, bool) { ++callbacks; };
    events.destroyedGenerator = [&](s32) { ++callbacks; };
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.help = [&](s32, usize) {
        ++callbacks;
        return true;
    };
    const auto advance = [&](OpponentsFixture& f, s32 tick, bool wrong) {
        if (tick % 11 == 0) {
            f.opponents.enemies().spawn(EnemySpawn{.kind = kGruntKind,
                                                   .tier = 1,
                                                   .position = {static_cast<f32>(tick), 0, 10},
                                                   .placed = true},
                                        {});
        }
        // Empty players keep the test's external player owner immutable. Native
        // population clocks, deaths, effects and coordinator output still advance.
        f.opponents.update(1, 1.0f / 60, {}, {}, opponentHost, opponentHost, opponentHost);
        if (wrong || tick % 7 == 0) {
            f.opponents.strikeGenerator(0, wrong ? 100000.0f : 1.0f, 2, f.players);
            f.opponents.strikeEnemy(0, wrong ? 100000.0f : 1.0f, 0, {0, 0, wrong ? -1.0f : 1.0f}, 2,
                                    f.players);
        }
    };
    const auto uploads = predicted->device.texturesCreated;
    s32 tick = 0;
    for (s32 window = 0; window < 24; ++window) {
        CAPTURE(window, tick);
        const auto enemies = swarmDigest(predicted->opponents, predicted->opponents.enemies());
        const auto generators =
            swarmDigest(predicted->opponents, predicted->opponents.generators());
        REQUIRE(predicted->opponents.saveLocal(*saved));
        predicted->effects.saveLocal(effects);
        const s32 distance = 1 + window % 8;
        for (s32 step = 0; step < distance; ++step) {
            advance(*predicted, tick + step, false);
            REQUIRE(predicted->opponents.saveLocal(expected[static_cast<usize>(step)]));
        }
        REQUIRE(predicted->opponents.restoreLocal(*saved));
        REQUIRE(predicted->effects.restoreLocal(effects));
        for (s32 step = 0; step < distance; ++step) {
            advance(*predicted, tick + step, true);
        }
        // The wrong branch must end somewhere else, or the replay below proves nothing.
        REQUIRE(predicted->opponents.saveLocal(*actual));
        CHECK_FALSE(
            predicted->opponents.equalLocal(expected[static_cast<usize>(distance - 1)], *actual));
        predicted->opponents.takeRockHits();
        predicted->opponents.takeBarrelRams();
        predicted->opponents.settleRewards({}, opponentHost, opponentHost);
        const auto calls = callbacks;
        REQUIRE(predicted->opponents.restoreLocal(*saved));
        REQUIRE(predicted->effects.restoreLocal(effects));
        CHECK(callbacks == calls);
        CHECK(swarmDigest(predicted->opponents, predicted->opponents.enemies()) == enemies);
        CHECK(swarmDigest(predicted->opponents, predicted->opponents.generators()) == generators);
        REQUIRE(predicted->opponents.saveLocal(*actual));
        CHECK(predicted->opponents.equalLocal(*saved, *actual));
        for (s32 step = 0; step < distance; ++step, ++tick) {
            advance(*reference, tick, false);
            advance(*predicted, tick, false);
            REQUIRE(predicted->opponents.saveLocal(*actual));
            CHECK(predicted->opponents.equalLocal(expected[static_cast<usize>(step)], *actual));
            CHECK(followupDigest(predicted->opponents) == followupDigest(reference->opponents));
            CHECK(swarmDigest(predicted->opponents, predicted->opponents.enemies()) ==
                  swarmDigest(reference->opponents, reference->opponents.enemies()));
            CHECK(swarmDigest(predicted->opponents, predicted->opponents.generators()) ==
                  swarmDigest(reference->opponents, reference->opponents.generators()));
            CHECK(swarmDigest(predicted->opponents, predicted->opponents.missiles()) ==
                  swarmDigest(reference->opponents, reference->opponents.missiles()));
            CHECK(test::liveDigest(predicted->opponents.placements()) ==
                  test::liveDigest(reference->opponents.placements()));
        }
        REQUIRE(predicted->opponents.canRestoreLocal(*saved));
    }
    CHECK(callbacks > 0);
    CHECK(predicted->device.texturesCreated == uploads);
}

TEST_CASE("native opponent aggregate preflights every child before mutation",
          "[level-opponents-local-rewind][native-equality]") {
    auto fixture = std::make_unique<OpponentsFixture>();
    auto foreign = std::make_unique<OpponentsFixture>();
    auto saved = std::make_unique<LevelOpponents::LocalSnapshot>();
    CHECK_FALSE(fixture->opponents.equalLocal(*saved, *saved));
    CHECK_FALSE(fixture->opponents.restoreLocal(*saved));
    REQUIRE(fixture->opponents.saveLocal(*saved));
    CHECK(fixture->opponents.equalLocal(*saved, *saved));
    CHECK_FALSE(foreign->opponents.equalLocal(*saved, *saved));
    CHECK_FALSE(foreign->opponents.restoreLocal(*saved));
    REQUIRE(fixture->opponents.restoreLocal(*saved));
    REQUIRE(fixture->opponents.restoreLocal(*saved));
    SECTION("child placement roster changed") {
        fixture->opponents.placements().bind({});
    }
    SECTION("child generator bindings changed") {
        fixture->opponents.generators().clear();
    }
    SECTION("child meter layout changed") {
        fixture->opponents.meter().clear();
    }
    SECTION("owner closed") {
        fixture->opponents.close();
    }
    const auto before = swarmDigest(fixture->opponents, fixture->opponents.enemies());
    CHECK_FALSE(fixture->opponents.canRestoreLocal(*saved));
    CHECK_FALSE(fixture->opponents.equalLocal(*saved, *saved));
    CHECK_FALSE(fixture->opponents.restoreLocal(*saved));
    CHECK(swarmDigest(fixture->opponents, fixture->opponents.enemies()) == before);
}

TEST_CASE("native opponent equality includes undispatched coordinator rewards",
          "[level-opponents-local-rewind][native-equality]") {
    auto fixture = std::make_unique<OpponentsFixture>();
    fixture->opponents.strikeGenerator(0, 1, 2, fixture->players);
    REQUIRE_FALSE(fixture->opponents.pendingGeneratorRewards().empty());
    auto before = std::make_unique<LevelOpponents::LocalSnapshot>();
    auto after = std::make_unique<LevelOpponents::LocalSnapshot>();
    REQUIRE(fixture->opponents.saveLocal(*before));
    auto enemiesBefore = std::make_unique<Enemies::LocalSnapshot>();
    auto enemiesAfter = std::make_unique<Enemies::LocalSnapshot>();
    Generators::LocalSnapshot generatorsBefore;
    Generators::LocalSnapshot generatorsAfter;
    REQUIRE(fixture->opponents.enemies().saveLocal(*enemiesBefore));
    fixture->opponents.generators().saveLocal(generatorsBefore);
    usize awards = 0;
    test::OpponentScript events;
    test::ScriptedOpponents opponentHost{events};
    events.award = [&](s32, s32, bool) { ++awards; };
    events.levels = [] {};
    fixture->opponents.settleRewards({}, opponentHost, opponentHost);
    CHECK(awards > 0);
    REQUIRE(fixture->opponents.saveLocal(*after));
    CHECK_FALSE(fixture->opponents.equalLocal(*before, *after));
    REQUIRE(fixture->opponents.enemies().saveLocal(*enemiesAfter));
    fixture->opponents.generators().saveLocal(generatorsAfter);
    CHECK(fixture->opponents.enemies().equalLocal(*enemiesBefore, *enemiesAfter));
    CHECK(fixture->opponents.generators().equalLocal(generatorsBefore, generatorsAfter));
    const auto dispatched = awards;
    REQUIRE(fixture->opponents.restoreLocal(*before));
    REQUIRE(fixture->opponents.saveLocal(*after));
    CHECK(fixture->opponents.equalLocal(*before, *after));
    CHECK(awards == dispatched); // value comparison and restore cannot dispatch rewards
}

} // namespace
