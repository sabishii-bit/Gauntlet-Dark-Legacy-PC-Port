#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/Generators.h"
#include "game/enemies/OpponentSimulation.h"
#include "game/enemies/PlacedEnemies.h"
#include "game/party/BodyCollision.h"
#include "game/players/PlayerActor.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Island {
    test::FakeRenderDevice device;
    WorldLayout layout;
    WorldCollision collision;
    Enemies enemies;
    Generators generators;
    EnemyMissiles missiles{45};
    OpponentSimulation simulation{generators, enemies, missiles};
    std::array<EnemyView, 1> players;
    std::vector<EnemyMissiles::Resource> resources;

    /** The pools' native values at one moment, and the placements' when they take part. */
    struct Saved {
        Generators::LocalSnapshot generators;
        std::unique_ptr<Enemies::LocalSnapshot> enemies =
            std::make_unique<Enemies::LocalSnapshot>();
        EnemyMissiles::LocalSnapshot missiles;
        PlacedEnemies::LocalSnapshot placements;
    };

    explicit Island(bool brood = false) {
        const auto root =
            test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
        const auto directory = test::scratchDirectory("opponent-simulation");
        writeTextFile(directory / "world.json",
                      brood ? R"({"objects":[{"name":"FLOOR","position":[0,0,0]}],
            "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,"hitPoints":10}],
            "itemInstances":[{"info":0,"minPlayers":1,"position":[60,0,60],
                              "params":[1,0,7,0,5,0,20,0,0,0,0,0]}]})"
                            : R"({"objects":[{"name":"FLOOR","position":[0,0,0]}]})");
        REQUIRE(layout.load(directory));
        collision.build(
            {{{0, 1, 0}, {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{200, 0, -200}}, 0, 4},
             {{0, 1, 0}, {Vec3{-200, 0, -200}, Vec3{-200, 0, 200}, Vec3{200, 0, 200}}, 0, 4}});
        enemies.open(device, root, &collision, Enemies::kMost, {}, 1125);
        REQUIRE(enemies.loadKind(kGruntKind));
        REQUIRE(generators.bind(device, layout, enemies, &collision, {}, 1));
        for (s32 slot = 0; slot < 3; ++slot) {
            const auto* model = enemies.projectileModel(kGruntKind, slot);
            if (model != nullptr && std::ranges::none_of(resources, [model](const auto& resource) {
                    return resource.model == model;
                })) {
                resources.push_back({static_cast<u32>(resources.size()) + 1, model});
            }
        }
        players[0].player = 0;
        players[0].position = {0, 0, 0};
    }

    Saved save(const PlacedEnemies* placements = nullptr) const {
        Saved saved;
        generators.saveLocal(saved.generators);
        REQUIRE(enemies.saveLocal(*saved.enemies));
        missiles.saveLocal(saved.missiles);
        if (placements != nullptr) {
            placements->saveLocal(saved.placements);
        }
        return saved;
    }
    void restore(const Saved& saved, PlacedEnemies* placements = nullptr) {
        REQUIRE(generators.restoreLocal(saved.generators));
        REQUIRE(enemies.restoreLocal(*saved.enemies));
        REQUIRE(missiles.restoreLocal(saved.missiles));
        if (placements != nullptr) {
            REQUIRE(placements->restoreLocal(saved.placements));
        }
    }
    /** Whether the pools (and `placements`) hold exactly what `saved` does. */
    bool holds(const Saved& saved, const PlacedEnemies* placements = nullptr) const {
        const auto now = save(placements);
        return generators.equalLocal(saved.generators, now.generators) &&
               enemies.equalLocal(*saved.enemies, *now.enemies) &&
               missiles.equalLocal(saved.missiles, now.missiles) &&
               (placements == nullptr || placements->equalLocal(saved.placements, now.placements));
    }

    OpponentSimulation::Frame frame() const {
        OpponentSimulation::Frame result;
        result.players = players;
        result.collision = &collision;
        return result;
    }

    s32 spawn(const Vec3& position, s32 way = kChaseWay, s32 tier = 1) {
        const auto slot = enemies.spawn({.kind = kGruntKind,
                                         .tier = tier,
                                         .algorithm = way,
                                         .position = position,
                                         .placed = true},
                                        players);
        REQUIRE(slot);
        return *slot;
    }

    // The pre-extraction LevelOpponents phase is kept as an independent oracle.
    void native(const OpponentSimulation::Frame& frame,
                const EnemyMissiles::SceneryBlast& scenery = {}) {
        generators.update(frame.ticks, enemies, frame.players, frame.fixtures, frame.timeStopped);
        auto boxes = generators.enemyObstacles();
        boxes.insert(boxes.end(), frame.fixtures.begin(), frame.fixtures.end());
        enemies.update(frame.ticks, frame.seconds, frame.players, boxes, &missiles,
                       frame.missileSpeed, frame.timeStopped);
        std::vector<MissileStop> stops;
        for (const auto& box : generators.obstacles()) {
            stops.push_back(MissileStop::of(box));
        }
        if (frame.stops.empty()) {
            for (const auto& box : frame.fixtures) {
                stops.push_back(MissileStop::of(box));
            }
        } else {
            stops.insert(stops.end(), frame.stops.begin(), frame.stops.end());
        }
        auto targets = enemies.targets();
        for (auto target : frame.critters) {
            target.id += 3000;
            targets.push_back(target);
        }
        for (auto target : frame.bosses) {
            target.id += 4000;
            targets.push_back(target);
        }
        missiles.update(frame.seconds, frame.collision, frame.players, targets, stops, scenery);
    }
};

TEST_CASE("opponent island preserves the native generator enemy and missile phase",
          "[diagnostics][opponent-simulation][assets]") {
    Island island(true);
    island.spawn({-5, 0, -18}, kThrowWay, kArcherStrength);
    const auto initial = island.save();
    const auto swarmBefore = island.enemies.checkpoint();
    const auto shotsBefore = island.missiles.checkpoint(island.resources);
    REQUIRE(swarmBefore);
    REQUIRE(shotsBefore);
    const auto uploads = island.device.texturesCreated;
    Obstacle box;
    box.centre = {5, 0, 6};
    box.halfAcross = 2;
    box.halfAlong = 1;
    box.height = 6;
    const std::array fixtures{box};
    const std::array stops{MissileStop::of(box)};
    const std::array critters{
        MissileTarget{.id = 2, .base = {20, 0, 20}, .radius = 1, .height = 4}};
    const std::array bosses{MissileTarget{.id = 3, .base = {40, 0, 40}, .radius = 2, .height = 8}};
    auto frame = island.frame();
    frame.fixtures = fixtures;
    frame.critters = critters;
    frame.bosses = bosses;
    std::vector<Island::Saved> expected;
    for (s32 tick = 0; tick < 150; ++tick) {
        frame.timeStopped = tick >= 40 && tick < 50;
        frame.stops =
            tick % 2 == 0 ? std::span<const MissileStop>{stops} : std::span<const MissileStop>{};
        island.native(frame);
        expected.push_back(island.save());
    }
    const auto swarmAfter = island.enemies.checkpoint();
    const auto shotsAfter = island.missiles.checkpoint(island.resources);
    REQUIRE(swarmAfter);
    REQUIRE(shotsAfter);
    REQUIRE(swarmAfter->nextInstance > swarmBefore->nextInstance);
    REQUIRE(shotsAfter->nextInstance > shotsBefore->nextInstance);
    island.restore(initial);
    for (s32 tick = 0; tick < 150; ++tick) {
        CAPTURE(tick);
        frame.timeStopped = tick >= 40 && tick < 50;
        frame.stops =
            tick % 2 == 0 ? std::span<const MissileStop>{stops} : std::span<const MissileStop>{};
        island.simulation.advance(frame);
        CHECK(island.holds(expected[static_cast<usize>(tick)]));
    }
    const auto roster = OpponentSimulation::targets(island.enemies, critters, bosses);
    REQUIRE(roster.size() >= 2);
    CHECK(roster[roster.size() - 2].id == 3002);
    CHECK(roster.back().id == 4003);
    CHECK(island.device.texturesCreated == uploads);
}

TEST_CASE("birth publication occurs after native AI and before missile movement",
          "[opponent-simulation][enemy-birth-emission][assets]") {
    Island island;
    island.spawn({0, 0, 40});
    island.missiles.launch(EnemyMissileKind::arrow(), {80, 10, 80}, {80, 10, 100}, 1, nullptr, -1);
    const auto swarm = island.enemies.checkpoint();
    REQUIRE(swarm);
    REQUIRE_FALSE(swarm->births.empty());
    REQUIRE(island.missiles.count() != 0);
    EnemyMissiles::LocalSnapshot shots;
    island.missiles.saveLocal(shots);
    usize called = 0;
    island.simulation.advance(island.frame(), {}, [&] {
        ++called;
        const auto afterAi = island.enemies.checkpoint();
        REQUIRE(afterAi);
        CHECK(afterAi->pendingTicks != swarm->pendingTicks);
        EnemyMissiles::LocalSnapshot unmoved;
        island.missiles.saveLocal(unmoved);
        CHECK(island.missiles.equalLocal(shots, unmoved));
        CHECK(island.enemies.takeBirths() == swarm->births);
    });
    CHECK(called == 1);
    const auto advanced = island.enemies.checkpoint();
    REQUIRE(advanced);
    CHECK(advanced->births.empty());
    EnemyMissiles::LocalSnapshot moved;
    island.missiles.saveLocal(moved);
    CHECK_FALSE(island.missiles.equalLocal(shots, moved));
}

TEST_CASE("native opponent motion updates player collision",
          "[diagnostics][opponent-simulation][assets]") {
    Island island;
    island.players[0].position = {0, 0, -100};
    const s32 slot = island.spawn({0, 0, 10});
    for (s32 tick = 0; tick < 60; ++tick) {
        island.native(island.frame());
    }
    island.players[0].position = {0, 0, 0};
    const auto baseline = island.save();
    const Vec3 start = island.enemies.positionOf(slot);
    const auto frozen = island.enemies.targets();
    REQUIRE(frozen.size() == 1);
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    const Vec3 desired{0, 0, 12};
    const Vec3 stale = BodyCollision::resolveMovement(actor, actor.position(), desired, frozen, {},
                                                      &island.collision);
    bool collisionChanged = false;
    for (s32 tick = 0; tick < 90; ++tick) {
        const auto before = island.save();
        island.native(island.frame());
        const auto native = island.save();
        const auto nativeTargets = island.enemies.targets();
        const Vec3 expected = BodyCollision::resolveMovement(actor, actor.position(), desired,
                                                             nativeTargets, {}, &island.collision);
        island.restore(before);
        island.simulation.advance(island.frame());
        CHECK(island.holds(native));
        const Vec3 resolved = BodyCollision::resolveMovement(
            actor, actor.position(), desired, island.enemies.targets(), {}, &island.collision);
        CHECK(glm::distance(resolved, expected) < 0.00001f);
        collisionChanged = collisionChanged || glm::distance(resolved, stale) > 0.01f;
    }
    REQUIRE(collisionChanged);
    CHECK(island.enemies.positionOf(slot) != start);
    // Body resolution changes actual movement without mutating the character save.
    const auto save = actor.save().toJson();
    actor.place(BodyCollision::resolveMovement(actor, actor.position(), desired,
                                               island.enemies.targets(), {}, &island.collision));
    CHECK(glm::distance(actor.position(), stale) > 0.01f);
    CHECK(actor.save().toJson() == save);
    island.restore(baseline);
    CHECK(island.holds(baseline));
}

TEST_CASE("opponent phase advances native flights and chasing bodies",
          "[diagnostics][opponent-simulation][assets]") {
    Island island;
    island.players[0].position = {0, 0, -100};
    const auto slot = island.spawn({0, 0, 20});
    for (s32 tick = 0; tick < 60; ++tick) {
        island.native(island.frame());
    }
    island.players[0].position = {0, 0, 0};
    island.missiles.launch(EnemyMissileKind::arrow(), {80, 10, 80}, {80, 10, 100}, 1, nullptr, -1);
    REQUIRE(island.missiles.count() == 1);
    const Vec3 shot = island.missiles.missile(0).position;
    const Vec3 body = island.enemies.positionOf(slot);
    for (s32 tick = 0; tick < 12; ++tick) {
        CAPTURE(tick);
        const auto before = island.save();
        island.native(island.frame());
        const auto expected = island.save();
        island.restore(before);
        island.simulation.advance(island.frame());
        CHECK(island.holds(expected));
    }
    REQUIRE(island.missiles.count() == 1);
    CHECK(island.missiles.missile(0).position != shot);
    CHECK(island.enemies.positionOf(slot) != body);
}

TEST_CASE("opponent phase preserves native shooting minds and pending throws",
          "[diagnostics][opponent-simulation][assets]") {
    Island island(true);
    SECTION("shooting native mind") {
        const auto way = GENERATE(kThrowWay, kBombWay, kSkirmishWay, kSkirmishBombWay,
                                  kStandCastWay, kRangeCastWay, kCastWay);
        island.spawn({0, 0, 20}, way, kArcherStrength);
    }
    SECTION("previously requested throw survives a change to a chasing mind") {
        const auto slot = island.spawn({0, 0, 20});
        auto state = island.enemies.checkpoint();
        REQUIRE(state);
        auto& animator = state->enemies[static_cast<usize>(slot)].animator;
        REQUIRE(animator);
        animator->requested = EnemyAction::Throw;
        REQUIRE(island.enemies.restore(*state));
    }
    SECTION("brood can create a shooting mind in the same update") {
        auto state = island.generators.checkpoint();
        REQUIRE(state);
        REQUIRE(state->generators.size() == 1);
        state->generators.front().algorithm = kThrowWay;
        REQUIRE(island.generators.restore(*state));
    }
    const auto before = island.save();
    island.native(island.frame());
    const auto expected = island.save();
    island.restore(before);
    island.simulation.advance(island.frame());
    CHECK(island.holds(expected));
}

TEST_CASE("archer and bomber releases match native phase through the first impact",
          "[diagnostics][opponent-simulation][enemy-launch][assets]") {
    const bool bred = GENERATE(false, true);
    const bool bomb = GENERATE(false, true);
    const s32 ticks = GENERATE(1, 2);
    CAPTURE(bred, bomb, ticks);
    Island island(bred);
    const s32 way = bomb ? kBombWay : kThrowWay;
    const s32 strength = bomb ? kBomberStrength : kArcherStrength;
    if (bred) {
        // Humanoids emerge ahead of the hut. Put the target on that same side
        // so the native launch sweep does not terminate inside the generator.
        island.players[0].position = {60, 0, 85};
        auto state = island.generators.checkpoint();
        REQUIRE(state);
        state->generators.front().algorithm = way;
        state->generators.front().tier = strength;
        REQUIRE(island.generators.restore(*state));
    } else {
        island.spawn({0, 0, -20}, way, strength);
    }
    auto frame = island.frame();
    frame.ticks = ticks;
    frame.seconds = static_cast<f32>(ticks) / 60;
    const auto baseline = island.missiles.checkpoint(island.resources);
    REQUIRE(baseline);
    const auto uploads = island.device.texturesCreated;
    bool released = false;
    bool reachedBoundary = false;
    for (s32 tick = 0; tick < 600; tick += ticks) {
        CAPTURE(bred, bomb, tick, ticks);
        const auto before = island.save();
        island.native(frame);
        const auto expected = island.save();
        const auto shots = island.missiles.checkpoint(island.resources);
        const auto swarm = island.enemies.checkpoint();
        REQUIRE(shots);
        REQUIRE(swarm);
        island.restore(before);
        island.simulation.advance(frame);
        CHECK(island.holds(expected));
        if (!shots->hits.empty() || !swarm->blows.empty()) {
            reachedBoundary = true;
            break;
        }
        if (shots->nextInstance > baseline->nextInstance) {
            released = true;
            REQUIRE_FALSE(shots->missiles.empty());
            CHECK(shots->missiles.front().kind.slot ==
                  (bomb ? EnemyMissileKind::kBomb : EnemyMissileKind::kArrow));
        }
    }
    CHECK(released);
    CHECK(reachedBoundary);
    CHECK(island.device.texturesCreated == uploads);
}

TEST_CASE("native opponent phase still dispatches scenery in its missile phase",
          "[diagnostics][opponent-simulation][assets]") {
    Island island;
    island.missiles.burst({0, 0, 0}, 10, 5, 0, 1, -1);
    const auto before = island.save();
    usize called = 0;
    island.simulation.advance(island.frame(), [&](const auto&, auto& contacts) {
        ++called;
        contacts.push_back(12);
    });
    CHECK(called == 1);
    REQUIRE(island.missiles.bursts().size() == 1);
    CHECK(island.missiles.bursts().front().scenery == std::vector<s32>{12});
    island.restore(before);
    CHECK(called == 1);
}

PlacedEnemies::Placement ordinaryPlacement(Vec3 position, s32 kind = kGruntKind) {
    PlacedEnemies::Placement row;
    row.kind = kind;
    row.spawn.kind = kind;
    row.spawn.algorithm = kChaseWay;
    row.spawn.position = position;
    row.spawn.placed = true;
    row.spawn.priority = EnemySpawn::Priority::Visible;
    row.viewRadius = 1;
    return row;
}

TEST_CASE("ordinary placements run their native watch phase before generators and swarm",
          "[diagnostics][opponent-simulation][opponent-placements][assets]") {
    Island island(true);
    PlacedEnemies placements;
    placements.bind({ordinaryPlacement({0, 0, 20}), ordinaryPlacement({8, 0, 25}),
                     ordinaryPlacement({0, 0, 80})});
    placements.observe(ViewVolume{}, {});
    OpponentSimulation combined{island.generators, island.enemies, island.missiles, &placements};
    const auto initial = island.save(&placements);
    const auto swarmBefore = island.enemies.checkpoint();
    REQUIRE(swarmBefore);
    const auto uploads = island.device.texturesCreated;
    std::vector<Island::Saved> expected;
    std::vector<usize> firstPending;
    std::optional<Enemies::Checkpoint> firstSwarm;
    for (s32 tick = 0; tick < 12; ++tick) {
        // Independent native LevelOpponents::watch -> update order; no replay
        // helper participates in the expected activation or motion.
        placements.activate([&](const auto& row) {
            island.enemies.spawn(row.spawn, {}, island.generators.obstacles());
        });
        island.native(island.frame());
        expected.push_back(island.save(&placements));
        if (tick == 0) {
            firstPending.assign(placements.pending().begin(), placements.pending().end());
            firstSwarm = island.enemies.checkpoint();
        }
    }
    REQUIRE(firstPending == std::vector<usize>{2});
    REQUIRE(firstSwarm);
    REQUIRE(firstSwarm->nextInstance > swarmBefore->nextInstance);
    island.restore(initial, &placements);
    for (usize tick = 0; tick < expected.size(); ++tick) {
        CAPTURE(tick);
        combined.advance(island.frame());
        CHECK(island.holds(expected[tick], &placements));
    }
    CHECK(island.device.texturesCreated == uploads);
}

} // namespace
