#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/File.h"

#include "game/enemies/OpponentCombatCheckpoint.h"
#include "game/enemies/PlacedEnemiesCheckpoint.h"
#include "game/enemies/ScriptedOpponents.h"
#include "game/hud/HelpMessages.h"
#include "game/level/LevelCatalog.h"
#include "game/party/AttackCheckpointFixture.h"
#include "game/scenes/StageResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct AdapterFixture {
    std::filesystem::path root = test::attackCheckpointAssets();
    test::FakeRenderDevice device;
    LevelWorld world;
    WorldLayout layout;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    LevelOpponents opponents;
    OpponentCombatCheckpoint adapter{opponents};

    AdapterFixture() {
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
    AdapterFixture(const AdapterFixture&) = delete;
    AdapterFixture& operator=(const AdapterFixture&) = delete;
    ~AdapterFixture() { opponents.close(); }
    EffectTrees::PoolCheckpoint visual() const {
        const auto result = effects.checkpointPool();
        REQUIRE(result);
        return *result;
    }
    OpponentCombatCheckpoint::State state() const {
        const auto result = adapter.checkpoint(visual());
        REQUIRE(result);
        return *result;
    }
};

TEST_CASE("ordinary opponent adapter restores every scalar and ordered pending output",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    const auto before = fixture.state();
    auto desired = before;
    desired.hitStreak = {1, 3, 9, 11};
    desired.lessons = {{HelpMessages::kDestroyGenerators, 2},
                       {HelpMessages::kDestroyGenerators, 1}};
    desired.generatorRewards = {{2, 150, false}, {1, 450, true}, {2, 25, false}};
    desired.rockHits = {{3, 5}, {1, 10}, {3, 2}};
    desired.hearers = {{1, 2, 3}, {-4, 0, 1}};
    desired.cueEffects = {3, 1};
    desired.shrink = 0.667f;
    desired.bagSeed = 0xF03454U;
    auto visual = fixture.visual();
    visual.nextId = 4; // three previously issued cues have already expired
    REQUIRE(desired.valid());
    auto prepared = fixture.adapter.prepareRestore(desired, visual);
    REQUIRE(prepared);
    CHECK(fixture.state() == before);
    CHECK(fixture.effects.count() == 0);
    REQUIRE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK(fixture.adapter.checkpoint(visual) == desired);
    CHECK(fixture.effects.count() == 0); // restore never recreates consumed visual work
    CHECK(fixture.opponents.takeRockHits() == desired.rockHits);
    CHECK(fixture.opponents.takeRockHits().empty());
    CHECK_FALSE(fixture.adapter.canCommit(*prepared));
}

TEST_CASE("ordinary opponent adapter preparation is owner bound and atomically validated",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    const auto before = fixture.state();
    auto invalid = before;
    switch (GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8)) {
    case 0: invalid.hitStreak[1] = -1; break;
    case 1: invalid.lessons = {{-1, 1}}; break;
    case 2: invalid.generatorRewards = {{4, 10, false}}; break;
    case 3: invalid.rockHits = {{0, std::numeric_limits<f32>::quiet_NaN()}}; break;
    case 4: invalid.hearers = {{std::numeric_limits<f32>::infinity(), 0, 0}}; break;
    case 5: invalid.cueEffects = {0}; break;
    case 6: invalid.shrink = 0; break;
    case 7: invalid.cueEffects = {1, 1}; break;
    case 8: invalid.lessons.resize(OpponentCombatCheckpoint::State::kMaxRecords + 1); break;
    default: break;
    }
    CHECK_FALSE(fixture.adapter.prepareRestore(invalid, fixture.visual()));
    CHECK_FALSE(fixture.adapter.restore(invalid, fixture.visual()));
    CHECK(fixture.state() == before);
}

TEST_CASE("ordinary adapter rejects future cue handles but retains native expired references",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    auto desired = fixture.state();
    auto visual = fixture.visual();
    desired.cueEffects = {1};
    REQUIRE(desired.valid());
    CHECK_FALSE(fixture.adapter.prepareRestore(desired, visual));
    visual.nextId = 2;
    REQUIRE(fixture.adapter.restore(desired, visual));
    CHECK(fixture.adapter.checkpoint(visual) == desired);
    visual.nextId = 1;
    CHECK_FALSE(fixture.adapter.checkpoint(visual));
}

TEST_CASE("ordinary adapter plans survive population correction but not close or reuse",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    OpponentCombatCheckpoint other{fixture.opponents};
    const auto before = fixture.state();
    auto desired = before;
    desired.hitStreak[2] = 7;
    auto prepared = fixture.adapter.prepareRestore(desired, fixture.visual());
    auto sibling = fixture.adapter.prepareRestore(desired, fixture.visual());
    REQUIRE(prepared);
    REQUIRE(sibling);
    CHECK_FALSE(other.commitRestore(std::move(*prepared)));
    REQUIRE(fixture.adapter.canCommit(*prepared));
    const auto population = fixture.opponents.enemies().checkpoint();
    REQUIRE(population);
    REQUIRE(fixture.opponents.enemies().restore(*population));
    REQUIRE(fixture.adapter.canCommit(*prepared));
    REQUIRE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK_FALSE(fixture.adapter.canCommit(*sibling));
    CHECK(fixture.state() == desired);
    prepared = fixture.adapter.prepareRestore(before, fixture.visual());
    REQUIRE(prepared);
    fixture.opponents.close();
    CHECK_FALSE(fixture.adapter.canCommit(*prepared));
    fixture.opponents.open({fixture.device, fixture.world, fixture.weapons, fixture.effects,
                            fixture.audio, fixture.root, 1},
                           fixture.players);
    CHECK_FALSE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK(fixture.state().hitStreak == std::array<s32, 4>{});
}

TEST_CASE("adapter cached capture matches fresh owners across clocks catalogs and rebinding",
          "[opponent-combat-checkpoint][checkpoint-capture-validation][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GOLEM.WAD").parent_path().parent_path();
    auto fixture = std::make_unique<AdapterFixture>();
    auto& critters = fixture->opponents.critters();
    critters.open(fixture->device, root, nullptr, {}, 'G');
    REQUIRE(critters.archiveFor(CombatantKind::Golem));
    const auto effects = fixture->visual();
    const auto compare = [&] {
        const auto uploads = fixture->device.texturesCreated;
        const auto captured = fixture->adapter.checkpoint(effects);
        REQUIRE(captured);
        REQUIRE(captured->valid());
        const OpponentCombatCheckpoint fresh{fixture->opponents};
        CHECK(fresh.checkpoint(effects) == captured);
        const CrittersCheckpoint independent{critters};
        CHECK(independent.checkpoint() == captured->critters);
        CHECK(fixture->opponents.statues().checkpoint() == captured->statues);
        CHECK(fixture->device.texturesCreated == uploads);
        return *captured;
    };
    const auto initial = compare();
    REQUIRE(initial.critters.banks.size() == 1);
    for (s32 tick = 0; tick < 12; ++tick) {
        critters.update(1, 1.0f / 60, {});
        compare();
    }
    // No actors or effects borrow this dormant bank. Reloading it in place
    // changes its resource token without changing the population's stock address.
    const auto stocks = critters.resources();
    REQUIRE(stocks.size() == 1);
    const auto definition = stocks.front()->definition;
    REQUIRE(stocks.front()->load(fixture->device, root, definition, 'G'));
    compare();
    auto stale = fixture->adapter.prepareRestore(initial, effects);
    REQUIRE(stale);
    REQUIRE(critters.archiveFor(CombatantKind::General));
    CHECK_FALSE(fixture->adapter.canCommit(*stale));
    CHECK(compare().critters.banks.size() == 2);

    // Native route replacement and population close/reopen must invalidate the
    // cached bindings, not return an old roster or dereference released stocks.
    critters.setLookouts({{{1, 0, 2}}, {65535}});
    CHECK(compare().critters.lookouts.points.size() == 1);
    critters.close();
    CHECK(compare().critters.banks.empty());
    critters.open(fixture->device, root, nullptr, {}, 'G');
    REQUIRE(critters.archiveFor(CombatantKind::Golem));
    const auto rebound = compare();
    REQUIRE(fixture->adapter.restore(rebound, effects));
    CHECK(compare() == rebound);
    CHECK(fixture->device.draws.empty());
}

TEST_CASE("adapter local capture keeps public child and supplied effect validation complete",
          "[opponent-combat-checkpoint][checkpoint-capture-validation]") {
    auto fixture = std::make_unique<AdapterFixture>();
    const auto baseline = fixture->state();
    const auto effects = fixture->visual();
    REQUIRE(baseline.valid());
    for (s32 mutation = 0; mutation < 4; ++mutation) {
        CAPTURE(mutation);
        auto invalid = baseline;
        switch (mutation) {
        case 0:
            invalid.statues.contactGridInverseWidth = std::numeric_limits<f32>::quiet_NaN();
            break;
        case 1: invalid.critters.actors.back().parts.front().arenaRandom = 0; break;
        case 2: invalid.projectiles.ricochetIn = std::numeric_limits<f32>::quiet_NaN(); break;
        case 3: invalid.shrink = std::numeric_limits<f32>::quiet_NaN(); break;
        default: break;
        }
        CHECK_FALSE(invalid.valid());
        CHECK_FALSE(fixture->adapter.prepareRestore(invalid, effects));
        CHECK(fixture->adapter.checkpoint(effects) == baseline);
    }
    auto invalidEffects = effects;
    REQUIRE(invalidEffects.effects.empty());
    invalidEffects.banks.push_back({0, {}});
    REQUIRE_FALSE(invalidEffects.valid());
    CHECK_FALSE(fixture->adapter.checkpoint(invalidEffects));
    CHECK_FALSE(fixture->adapter.prepareRestore(baseline, invalidEffects));
    CHECK(fixture->adapter.checkpoint(effects) == baseline);
}

TEST_CASE("native hit streak and generator rewards replay in their original settlement order",
          "[opponent-combat-checkpoint][generator-lesson]") {
    AdapterFixture fixture;
    const auto enemy = fixture.opponents.enemies().spawn(
        EnemySpawn{.kind = kGruntKind, .tier = 1, .generator = 0, .placed = true}, {});
    REQUIRE(enemy);
    for (s32 hit = 0; hit < 9; ++hit) {
        fixture.opponents.strikeEnemy(*enemy, 5, 0, {0, 0, 1}, 2, fixture.players);
    }
    const auto ninth = fixture.state();
    REQUIRE(ninth.hitStreak[2] == 9);
    REQUIRE(ninth.lessons.empty());
    fixture.opponents.strikeEnemy(*enemy, 5, 0, {0, 0, 1}, 2, fixture.players);
    fixture.opponents.strikeGenerator(0, 1, 1, fixture.players);
    fixture.opponents.strikeGenerator(0, 100000, 2, fixture.players);
    const auto queued = fixture.state();
    REQUIRE(queued.lessons ==
            std::vector<std::pair<s32, s32>>{{HelpMessages::kDestroyGenerators, 2}});
    REQUIRE(queued.generatorRewards.size() == 2);
    REQUIRE_FALSE(queued.generatorRewards[0].destroyed);
    REQUIRE(queued.generatorRewards[1].destroyed);
    CHECK(queued.hitStreak[2] == 0); // destruction resets only its hitter's native streak
    const auto swarm = fixture.opponents.enemies().checkpoint();
    REQUIRE(swarm);
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
    auto prepared = fixture.adapter.prepareRestore(queued, fixture.visual());
    REQUIRE(prepared);
    REQUIRE(fixture.opponents.enemies().restore(*swarm));
    REQUIRE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK(events.empty());
    fixture.opponents.settleRewards(fixture.players, opponentHost, opponentHost);
    CHECK(events == expected);
    events.clear();
    fixture.opponents.settleRewards(fixture.players, opponentHost, opponentHost);
    CHECK(events == std::vector<std::string>{"levels"});
}

TEST_CASE("ordinary adapter preserves dormant special placements without activating them",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    const s32 kind = GENERATE(kDeathKind, kItKind, kGarmBroodKind, kGeneralEnemyKind);
    PlacedEnemies::Placement placement;
    placement.kind = kind;
    placement.spawn.kind = kind;
    placement.spawn.placed = true;
    fixture.opponents.placements().bind({placement});
    REQUIRE(fixture.opponents.pendingPlacements() == 1);
    const auto pending = PlacedEnemiesCheckpoint::capture(fixture.opponents.placements());
    REQUIRE(pending);
    const auto adapter = fixture.adapter.checkpoint(fixture.visual());
    REQUIRE(adapter);
    REQUIRE(fixture.adapter.restore(*adapter, fixture.visual()));
    CHECK(PlacedEnemiesCheckpoint::capture(fixture.opponents.placements()) == pending);
    CHECK(fixture.opponents.pendingPlacements() == 1);
}

TEST_CASE("dormant family clocks resolve by trusted identity rather than load order",
          "[opponent-combat-checkpoint][dormant-opponents][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    AdapterFixture host;
    AdapterFixture guest;
    host.opponents.critters().open(host.device, root, nullptr, {}, 'G');
    guest.opponents.critters().open(guest.device, root, nullptr, {}, 'G');
    for (const auto kind : {CombatantKind::General, CombatantKind::Gargoyle}) {
        REQUIRE(host.opponents.critters().archiveFor(kind) != nullptr);
    }
    for (const auto kind : {CombatantKind::Gargoyle, CombatantKind::General}) {
        REQUIRE(guest.opponents.critters().archiveFor(kind) != nullptr);
    }
    REQUIRE(host.opponents.critters().count() == 0);
    REQUIRE(guest.opponents.critters().count() == 0);
    auto expected = host.state();
    REQUIRE(expected.critters.banks.size() == 2);
    expected.critters.banks[0].clock = {17, 0.25f, 0.5f};
    expected.critters.banks[1].clock = {101, 0.75f, 1};
    REQUIRE(expected.valid());
    REQUIRE(host.adapter.restore(expected, host.visual()));
    const auto uploads = guest.device.texturesCreated;
    REQUIRE(guest.adapter.restore(expected, guest.visual()));
    CHECK(guest.state() == host.state());
    for (s32 tick = 0; tick < 30; ++tick) {
        host.opponents.critters().update(1, 1.0f / 60, {});
        guest.opponents.critters().update(1, 1.0f / 60, {});
        CHECK(host.state() == guest.state());
    }
    CHECK(guest.device.texturesCreated == uploads);
    const auto before = guest.state();
    auto untrusted = before;
    untrusted.critters.banks.front().name = "UNBOUND";
    REQUIRE(untrusted.valid());
    CHECK_FALSE(guest.adapter.prepareRestore(untrusted, guest.visual()));
    CHECK(guest.state() == before);
    auto prepared = guest.adapter.prepareRestore(before, guest.visual());
    REQUIRE(prepared);
    REQUIRE(guest.opponents.critters().archiveFor(CombatantKind::Golem) != nullptr);
    CHECK_FALSE(guest.adapter.canCommit(*prepared));
    CHECK_FALSE(guest.adapter.commitRestore(std::move(*prepared)));
}

TEST_CASE("critter bank identities and counts are strictly bounded",
          "[opponent-combat-checkpoint][dormant-opponents]") {
    OpponentCombatCheckpoint::State state;
    state.critters.banks = {{1, CombatantKind::Golem, "GOLEM", "GOLEM", {}},
                            {2, CombatantKind::Gargoyle, "GAR_EAGL", "GAR_EAGL", {}}};
    state.projectiles.catalog = {{1, CombatantKind::Golem, "GOLEM", {}},
                                 {2, CombatantKind::Gargoyle, "GAR_EAGL", {}}};
    REQUIRE(state.valid());
    switch (GENERATE(0, 1, 2, 3, 4, 5)) {
    case 0: state.critters.banks[0].kind = CombatantKind::Boss; break;
    case 1: state.critters.banks[0].name.push_back('\0'); break;
    case 2: state.critters.banks[0].name.assign(65, 'A'); break;
    case 3: std::swap(state.critters.banks[0], state.critters.banks[1]); break;
    case 4: state.critters.banks[1] = state.critters.banks[0]; break;
    case 5: state.critters.banks.resize(CrittersCheckpoint::kMaxStocks + 1); break;
    default: break;
    }
    CHECK_FALSE(state.valid());
}

TEST_CASE("complete critter state does not authorize dormant-only simulation",
          "[opponent-combat-checkpoint][dormant-opponents]") {
    OpponentCombatCheckpoint::State state;
    REQUIRE(state.valid());
    REQUIRE(state.dormant());
    switch (GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)) {
    case 0: state.critters.actors[0].parts[0].actor.state = 1; break;
    case 1: state.critters.actors[0].parts[0].actor.definition.resource = 1; break;
    case 2: state.critters.actors[0].parts.emplace_back(); break;
    case 3: state.critters.actors[0].parts[0].events.cues.emplace_back(); break;
    case 4: state.critters.events.losses.emplace_back(); break;
    case 5: state.projectiles.flying.emplace_back(); break;
    case 6: state.projectiles.emittedEffects.push_back(1); break;
    case 7: state.projectiles.hits.emplace_back(); break;
    case 8: state.projectiles.worldHits.emplace_back(); break;
    case 9: state.projectiles.rockHits.emplace_back(); break;
    case 10: state.projectiles.generators.emplace_back(1); break;
    case 11: state.projectiles.summons.emplace_back(1); break;
    case 12: state.projectiles.ricochetIn = 0.1f; break;
    case 13: state.critterEffects.emplace_back(); break;
    case 14: state.moveEffects.emplace_back(); break;
    case 15: state.barrelRams.emplace_back(); break;
    default: break;
    }
    CHECK_FALSE(state.dormant());
    state = {};
    state.critterExperienceOwed[2] = 0.25f;
    CHECK_FALSE(state.dormant());
}

TEST_CASE("native G1 adapter preserves its complete dormant statues and preloaded family banks",
          "[opponent-combat-checkpoint][dormant-opponents][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, {10000, 0, 10000}, 0);
    players[1].actor.spawn(1, {}, nullptr, {10010, 0, 10000}, 0);
    world.setPlayerCount(2);
    LevelOpponents opponents;
    opponents.open({device, world, weapons, effects, audio, root, 1, true}, players);
    REQUIRE(StageResources::preload(world, opponents.enemies(), opponents.critters(),
                                    opponents.bosses()));
    OpponentCombatCheckpoint adapter{opponents};
    const auto visual = effects.checkpointPool();
    REQUIRE(visual);
    const auto before = adapter.checkpoint(*visual);
    REQUIRE(before);
    REQUIRE_FALSE(before->statues.roster.empty());
    REQUIRE_FALSE(before->statues.statues.empty());
    REQUIRE_FALSE(before->critters.banks.empty());
    REQUIRE(opponents.critters().count() == 0);
    const auto placements = PlacedEnemiesCheckpoint::capture(opponents.placements());
    REQUIRE(placements);
    REQUIRE_FALSE(placements->roster.empty());
    const auto uploads = device.texturesCreated;
    auto undo = adapter.prepareRestore(*before, *visual);
    REQUIRE(undo);
    opponents.critters().update(1, 1.0f / 60, {});
    opponents.wakeStatue(0);
    CHECK_FALSE(opponents.statues().dormantFor({}));
    const auto changed = adapter.checkpoint(*visual);
    REQUIRE(changed);
    CHECK(changed->statues.statues.front().woken);
    CHECK(changed->critters.banks != before->critters.banks);
    REQUIRE(adapter.canCommit(*undo));
    REQUIRE(adapter.commitRestore(std::move(*undo)));
    CHECK(adapter.checkpoint(*visual) == before);
    CHECK(PlacedEnemiesCheckpoint::capture(opponents.placements()) == placements);
    CHECK(device.texturesCreated == uploads);
    auto first = adapter.prepareRestore(*before, *visual);
    auto sibling = adapter.prepareRestore(*before, *visual);
    REQUIRE(first);
    REQUIRE(sibling);
    REQUIRE(adapter.commitRestore(std::move(*first)));
    CHECK_FALSE(adapter.canCommit(*sibling));
    opponents.close();
}

TEST_CASE("dormant statue admission uses the native grid wake and body predicates without mutation",
          "[opponent-combat-checkpoint][dormant-opponents][assets]") {
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
    const auto before = statues.checkpoint();
    REQUIRE(before);
    const bool quiet = statues.dormantFor(std::span{&visitor, 1});
    CHECK(statues.checkpoint() == before);
    const Vec3 after = statues.touch(visitor.position, visitor.radius, visitor.height);
    const bool changed = statues.woken(0) || after != visitor.position;
    CHECK(quiet == !changed);
    if (quiet) {
        statues.update(1, 1.0f / 60, [](const Vec3&, f32) { return true; });
        CHECK(statues.checkpoint() == before);
    } else {
        CHECK_FALSE(statues.dormantFor(std::span{&visitor, 1}));
    }
}

TEST_CASE("ordinary adapter refuses queued special-owner outcomes even with no visible body",
          "[opponent-combat-checkpoint]") {
    AdapterFixture fixture;
    const auto ordinary = fixture.state();
    auto prepared = fixture.adapter.prepareRestore(ordinary, fixture.visual());
    REQUIRE(prepared);
    auto swarm = fixture.opponents.enemies().checkpoint();
    REQUIRE(swarm);
    switch (GENERATE(0, 1, 2)) {
    case 0: swarm->deathEvents.push_back({DeathEvent::Kind::Killed, 0, 2}); break;
    case 1: swarm->deathShots.push_back({0}); break;
    case 2: swarm->tagged.push_back(2); break;
    default: break;
    }
    REQUIRE(swarm->valid());
    REQUIRE(fixture.opponents.enemies().restore(*swarm));
    REQUIRE(fixture.opponents.enemies().count() == 0);
    CHECK_FALSE(fixture.adapter.checkpoint(fixture.visual()));
    CHECK_FALSE(fixture.adapter.canCommit(*prepared));
    CHECK_FALSE(fixture.adapter.commitRestore(std::move(*prepared)));
    CHECK(fixture.opponents.enemies().checkpoint() == swarm);
}

TEST_CASE("opponent adapter preserves defeated critters and their pending rewards without dispatch",
          "[opponent-combat-checkpoint][assets]") {
    const auto native = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    AdapterFixture fixture;
    auto& critters = fixture.opponents.critters();
    critters.open(fixture.device, native, nullptr, {}, 'G');
    const auto general = critters.spawnGeneral({}, 0);
    REQUIRE(general);
    const auto alive = fixture.adapter.checkpoint(fixture.visual());
    REQUIRE(alive);
    auto undo = fixture.adapter.prepareRestore(*alive, fixture.visual());
    REQUIRE(undo);
    EnemyHit hit;
    hit.damage = 1000000;
    hit.player = 2;
    critters.hurt(*general, hit);
    REQUIRE(critters.dying(*general));
    for (s32 frame = 0; frame < 1200 && critters.count() != 0; ++frame) {
        critters.update(2, 1.0f / 30, {});
    }
    REQUIRE(critters.count() == 0);
    const auto dead = fixture.adapter.checkpoint(fixture.visual());
    REQUIRE(dead);
    CHECK_FALSE(dead->critters.events.losses.empty());
    REQUIRE(dead->critters.actors[static_cast<usize>(*general)].parts.size() == 1);
    const auto& retired = dead->critters.actors[static_cast<usize>(*general)].parts.front();
    CHECK(retired.actor.definition.resource == 0);
    CHECK(retired.id == *general);
    auto foreignSlot = *dead;
    foreignSlot.critters.actors[static_cast<usize>(*general)].parts.front().id = *general + 1;
    CHECK_FALSE(foreignSlot.valid());
    CHECK_FALSE(fixture.adapter.prepareRestore(foreignSlot, fixture.visual()));
    REQUIRE(fixture.adapter.canCommit(*undo));
    REQUIRE(fixture.adapter.commitRestore(std::move(*undo)));
    CHECK(fixture.adapter.checkpoint(fixture.visual()) == alive);
    REQUIRE(fixture.adapter.restore(*dead, fixture.visual()));
    CHECK_FALSE(critters.takeLosses().empty()); // Capture and restore never dispatch rewards.
    CHECK_FALSE(fixture.adapter.prepareRestore({}, fixture.visual()));
}

TEST_CASE("native opponent owner replays wrong branches through pools queues and rewards",
          "[level-opponents-local-rewind][native-equality]") {
    auto reference = std::make_unique<AdapterFixture>();
    auto predicted = std::make_unique<AdapterFixture>();
    const auto prepare = [](AdapterFixture& f) {
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
    REQUIRE_FALSE(predicted->state().lessons.empty());
    REQUIRE_FALSE(predicted->state().generatorRewards.empty());
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
    const auto advance = [&](AdapterFixture& f, s32 tick, bool wrong) {
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
        const auto before = predicted->state();
        const auto enemies = predicted->opponents.enemies().checkpoint();
        const auto generators = predicted->opponents.generators().checkpoint();
        REQUIRE(predicted->opponents.saveLocal(*saved));
        predicted->effects.saveLocal(effects);
        const s32 distance = 1 + window % 8;
        for (s32 step = 0; step < distance; ++step) {
            advance(*predicted, tick + step, false);
            REQUIRE(predicted->opponents.saveLocal(expected[static_cast<usize>(step)]));
        }
        REQUIRE(predicted->opponents.restoreLocal(*saved));
        REQUIRE(predicted->effects.restoreLocal(effects));
        auto diagnostic = predicted->adapter.prepareRestore(before, predicted->visual());
        REQUIRE(diagnostic);
        for (s32 step = 0; step < distance; ++step) {
            advance(*predicted, tick + step, true);
        }
        predicted->opponents.takeRockHits();
        predicted->opponents.takeBarrelRams();
        predicted->opponents.settleRewards({}, opponentHost, opponentHost);
        const auto calls = callbacks;
        REQUIRE(predicted->opponents.restoreLocal(*saved));
        REQUIRE(predicted->effects.restoreLocal(effects));
        CHECK(callbacks == calls);
        CHECK_FALSE(predicted->adapter.canCommit(*diagnostic));
        CHECK(predicted->state() == before);
        CHECK(predicted->opponents.enemies().checkpoint() == enemies);
        CHECK(predicted->opponents.generators().checkpoint() == generators);
        REQUIRE(predicted->opponents.saveLocal(*actual));
        CHECK(predicted->opponents.equalLocal(*saved, *actual));
        for (s32 step = 0; step < distance; ++step, ++tick) {
            advance(*reference, tick, false);
            advance(*predicted, tick, false);
            REQUIRE(predicted->opponents.saveLocal(*actual));
            CHECK(predicted->opponents.equalLocal(expected[static_cast<usize>(step)], *actual));
            CHECK(predicted->state() == reference->state());
            CHECK(predicted->opponents.enemies().checkpoint() ==
                  reference->opponents.enemies().checkpoint());
            CHECK(predicted->opponents.generators().checkpoint() ==
                  reference->opponents.generators().checkpoint());
            CHECK(predicted->opponents.missiles().checkpoint() ==
                  reference->opponents.missiles().checkpoint());
            CHECK(PlacedEnemiesCheckpoint::capture(predicted->opponents.placements()) ==
                  PlacedEnemiesCheckpoint::capture(reference->opponents.placements()));
        }
        REQUIRE(predicted->opponents.canRestoreLocal(*saved));
    }
    CHECK(callbacks > 0);
    CHECK(predicted->device.texturesCreated == uploads);
}

TEST_CASE("native opponent aggregate preflights every child before mutation",
          "[level-opponents-local-rewind][native-equality]") {
    auto fixture = std::make_unique<AdapterFixture>();
    auto foreign = std::make_unique<AdapterFixture>();
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
    SECTION("diagnostic coordinator replacement") {
        REQUIRE(fixture->adapter.restore(fixture->state(), fixture->visual()));
    }
    SECTION("owner closed") {
        fixture->opponents.close();
    }
    const auto before = fixture->opponents.enemies().checkpoint();
    CHECK_FALSE(fixture->opponents.canRestoreLocal(*saved));
    CHECK_FALSE(fixture->opponents.equalLocal(*saved, *saved));
    CHECK_FALSE(fixture->opponents.restoreLocal(*saved));
    CHECK(fixture->opponents.enemies().checkpoint() == before);
}

TEST_CASE("native opponent equality includes undispatched coordinator rewards",
          "[level-opponents-local-rewind][native-equality]") {
    auto fixture = std::make_unique<AdapterFixture>();
    fixture->opponents.strikeGenerator(0, 1, 2, fixture->players);
    REQUIRE_FALSE(fixture->state().generatorRewards.empty());
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
