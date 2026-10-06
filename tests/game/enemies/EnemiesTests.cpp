#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMind.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/HazardSurfaces.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;
constexpr Vec3 kFarEye{0.0f, 1.0e6f, 0.0f}; ///< so high that a shadow is pulled straight up

std::filesystem::path unpackedRoot() {
    return test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
}

TEST_CASE("enemy aim snapshots retain the native collision-height anchor",
          "[enemies][alpha-aim-acquisition][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 4, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.position = {10, 20, 30};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    const auto targets = enemies.targets();
    REQUIRE(targets.size() == 1);
    const auto& target = targets[0];
    REQUIRE(target.acquisition);
    const Vec3 expected =
        enemies.positionOf(*id) + Vec3{0, enemyKind(kGruntKind).collisionHeight, 0};
    CHECK(target.acquisition->point == expected);
    CHECK(target.acquisition->radius == enemies.radiusOf(*id));
    CHECK(target.acquisition->distanceScale == 1);
    CHECK(target.acquisition->maxHeight == 10);
    CHECK(target.base == enemies.positionOf(*id));
    CHECK(target.height == enemies.heightOf(*id));
}

TEST_CASE("recycling a brood slot releases its original generator even if replacement fails",
          "[enemies][generator-feedback][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.generator = 4;
    spawn.placed = true;
    REQUIRE(enemies.spawn(spawn, {}).has_value());
    const auto born = enemies.takeGeneratorEvents();
    REQUIRE(born.size() == 1);
    CHECK(born[0].generator == 4);
    CHECK(born[0].kind == EnemyGeneratorEvent::Kind::Born);
    spawn.generator = 7;
    spawn.priority = EnemySpawn::Priority::Visible;
    bool fails = false;
    SECTION("successful replacement") {}
    SECTION("all birth exits obstructed") {
        fails = true;
        spawn.placed = false;
    }
    const std::array players{EnemyView{.radius = 100}};
    CHECK(enemies.spawn(spawn, players).has_value() == !fails);
    const auto events = enemies.takeGeneratorEvents();
    REQUIRE(events.size() == (fails ? 1 : 2));
    CHECK(events[0].generator == 4);
    CHECK(events[0].kind == EnemyGeneratorEvent::Kind::Detached);
    if (!fails) {
        CHECK(events[1].generator == 7);
        CHECK(events[1].kind == EnemyGeneratorEvent::Kind::Born);
    }
    CHECK(enemies.takeGeneratorEvents().empty());
}

TEST_CASE("full enemy pools prefer replacing brood over authored sentries",
          "[enemies][generator-feedback][alpha-recycling][assets]") {
    // find_enemy_slot (main.dol 0x8004FD00) reduces birth_style != 0's
    // replacement score, independently of whether the sentry is asleep.
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 2, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn sentry;
    sentry.placed = true;
    sentry.patrolBirth = GENERATE(false, true);
    const auto sentryId = enemies.spawn(sentry, {});
    REQUIRE(sentryId);
    EnemySpawn brood;
    brood.generator = 4;
    brood.position = {20, 0, 0};
    const auto broodId = enemies.spawn(brood, {});
    REQUIRE(broodId);
    REQUIRE(*sentryId != *broodId);
    static_cast<void>(enemies.takeGeneratorEvents());

    EnemySpawn incoming;
    incoming.placed = true;
    incoming.position = {40, 0, 0};
    incoming.priority = EnemySpawn::Priority::Visible;
    CHECK(enemies.spawn(incoming, {}) == broodId);
    CHECK(enemies.positionOf(*sentryId) == sentry.position);
    const auto events = enemies.takeGeneratorEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].generator == 4);
    CHECK(events[0].kind == EnemyGeneratorEvent::Kind::Detached);
}

/** A one-triangle IT, enough to stand and walk. */
std::filesystem::path itArchive() {
    const auto root = test::scratchDirectory("it-enemy");
    const auto dir = root / "MONSTERS/IT";
    std::filesystem::create_directories(dir);
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(dir);
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"IT1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":13,"rate":30},
                     {"name":"WALK1","frames":13,"rate":30}]}]})");
    return root;
}

EnemyView playerAt(const Vec3& position, s32 player = 0) {
    EnemyView view;
    view.player = player;
    view.position = position;
    view.radius = 1.0f;
    view.height = 6.0f;
    return view;
}

TEST_CASE("swarm rendering uses its native pending phase without changing the live body",
          "[enemies][presentation][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.algorithm = 0;
    const std::array players{playerAt(Vec3{0, 0, 20})};
    const auto id = enemies.spawn(spawn, players);
    REQUIRE(id);
    for (s32 step = 0; step < 40; ++step) {
        enemies.update(2, 1.0f / 30, players);
    }
    const Vec3 live = enemies.positionOf(*id);
    const f32 frame = enemies.animatorOf(*id)->player().frame();
    const auto vertex = [&](f32 alpha) {
        device.draws.clear();
        enemies.draw(device, Mat4{1}, {}, nullptr, nullptr, nullptr, alpha);
        REQUIRE_FALSE(device.draws.empty());
        return device.draws[0].vertices[0].position;
    };
    const Vec3 first = vertex(0);
    const Vec3 half = vertex(1);
    CHECK(glm::distance(first, half) > 0.0001f);
    CHECK(enemies.positionOf(*id) == live);
    CHECK(enemies.animatorOf(*id)->player().frame() == frame);
    enemies.update(1, 1.0f / 60, players);
    CHECK(vertex(0) == half); // Same native interval, now in its second tick.
    CHECK(enemies.positionOf(*id) == live);
    const Vec3 end = vertex(1);
    CHECK(glm::distance(half, end) > 0.0001f);
    enemies.update(1, 1.0f / 60, players, {}, nullptr, 1, true);
    CHECK(vertex(0) == vertex(1)); // Stop time holds the pose, not the last moving interval.
    enemies.close();
}

CollisionTriangle triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& normal) {
    CollisionTriangle out;
    out.vertices = {a, b, c};
    out.normal = normal;
    out.object = 0;
    return out;
}

/** A floor at y 0 from x -40 to 30 with a wall across the middle, along x from -12 to 12 at
 * z 20, eight tall, and nothing at all past x 30 (a ledge). */
std::vector<CollisionTriangle> yard() {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 south{0.0f, 0.0f, -1.0f};
    const Vec3 north{0.0f, 0.0f, 1.0f};
    return {
        triangle({-40, 0, -40}, {30, 0, -40}, {30, 0, 40}, up),
        triangle({-40, 0, -40}, {30, 0, 40}, {-40, 0, 40}, up),
        triangle({-12, 0, 20}, {12, 8, 20}, {12, 0, 20}, south),
        triangle({-12, 0, 20}, {-12, 8, 20}, {12, 8, 20}, south),
        triangle({-12, 0, 20.5}, {12, 0, 20.5}, {12, 8, 20.5}, north),
        triangle({-12, 0, 20.5}, {12, 8, 20.5}, {-12, 8, 20.5}, north),
    };
}

s32 stepsUntil(Enemies& enemies, std::span<const EnemyView> players, const auto& done, s32 limit) {
    s32 steps = 0;
    while (!done() && steps < limit) {
        enemies.update(kTicks, kStep, players);
        ++steps;
    }
    return steps;
}

TEST_CASE("wandering bodies turn before their feet reach the wall detected ahead",
          "[enemies][wander-body-probe][assets]") {
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(yard());
    for (const s32 way : {kWanderWay, kWanderOtherWay}) {
        CAPTURE(way);
        Enemies enemies;
        enemies.open(device, unpackedRoot(), &collision, 1, {}, 1);
        REQUIRE(enemies.loadKind(kGruntKind));
        EnemySpawn spawn;
        spawn.placed = true;
        spawn.algorithm = way;
        spawn.position = {0, 0, 18.1f};
        const auto id = enemies.spawn(spawn, {});
        REQUIRE(id);
        enemies.update(kTicks, kStep, {});
        const f32 sign = way == kWanderWay ? -1.0f : 1.0f;
        CHECK(enemies.memoryOf(*id).heading == Approx(sign * 1.570796327f));
        CHECK(enemies.memoryOf(*id).deadEnd == 20);
        // The placement's initial hold keeps its feet still on this first tick;
        // the mind has already chosen the safe heading rather than waiting to hit.
        CHECK(enemies.positionOf(*id) == spawn.position);
    }
}

TEST_CASE("dog broods use only the two native birth directions while leader broods use one",
          "[enemies][brood-directions][assets]") {
    test::FakeRenderDevice device;
    constexpr s32 kDogKind = 18;
    constexpr s32 kLeaderWay = 12;
    for (const bool leader : {false, true}) {
        CAPTURE(leader);
        Enemies enemies;
        enemies.open(device, unpackedRoot(), nullptr, 1, {}, 1);
        EnemySpawn spawn;
        spawn.kind = leader ? kGruntKind : kDogKind;
        spawn.algorithm = leader ? kLeaderWay : kChaseWay;
        spawn.generator = 0;
        spawn.clearance = 4;
        REQUIRE(enemies.loadKind(spawn.kind));
        const f32 distance = spawn.clearance + enemyKind(spawn.kind).radius;
        std::vector<EnemyView> blockers{playerAt({0, 0, distance})};
        if (!leader) {
            blockers.push_back(playerAt({0, 0, -distance}, 1));
        }
        // The lateral openings do not authorize inventing another birth direction.
        CHECK_FALSE(enemies.spawn(spawn, blockers).has_value());
        const auto id = enemies.spawn(spawn, {});
        REQUIRE(id);
        CHECK(enemies.positionOf(*id).x == Approx(0));
        CHECK(std::abs(enemies.positionOf(*id).z) == Approx(distance));
        if (leader) {
            CHECK(enemies.positionOf(*id).z > 0);
        }
    }
}

TEST_CASE("swarm movement cannot pass through the larger creatures' solid collision nodes",
          "[enemies][combatant-obstacles][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    const bool above = GENERATE(false, true);
    const std::array bodies{MissileTarget{0, Vec3{0, above ? 30.0f : 0.0f, 7}, 3, 8}};
    enemies.setCombatantBodies(bodies);
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.algorithm = kChaseWay;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    const std::array players{playerAt({0, 0, 25})};
    f32 nearest = 1000;
    for (s32 frame = 0; frame < 240; ++frame) {
        enemies.update(kTicks, kStep, players);
        const Vec3 at = enemies.positionOf(*id);
        nearest = std::min(nearest, glm::length(Vec2{at.x, at.z - 7}));
    }
    if (above) {
        CHECK(nearest < 2); // An upper-storey creature does not block the floor below.
    } else {
        CHECK(nearest >= 3 + enemies.radiusOf(*id) - 0.01f);
        CHECK(enemies.positionOf(*id).z > 12); // Must route around, not deadlock against it.
    }
}

TEST_CASE("combatant collision rejects generator births and releases the space when removed",
          "[enemies][combatant-obstacles][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.clearance = 4;
    // Block all three permitted grunt birth directions from outside the bodies.
    // Retail deliberately permits an already-overlapping body to escape outward.
    const std::array bodies{MissileTarget{0, Vec3{0, 0, 5.5f}, 3, 8},
                            MissileTarget{1, Vec3{3.9f, 0, 3.9f}, 3, 8},
                            MissileTarget{2, Vec3{-3.9f, 0, 3.9f}, 3, 8}};
    enemies.setCombatantBodies(bodies);
    CHECK_FALSE(enemies.spawn(spawn, {}));
    enemies.setCombatantBodies({});
    CHECK(enemies.spawn(spawn, {}));
}

TEST_CASE("a knocked swarm enemy cannot tunnel through a combatant collision node",
          "[enemies][combatant-obstacles][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    const std::array bodies{MissileTarget{0, Vec3{0, 0, 3}, 1, 8}};
    enemies.setCombatantBodies(bodies);
    EnemyHit hit;
    hit.damage = 1;
    hit.flags = EnemyHit::kKnockBack;
    hit.direction = {0, 0, 1};
    enemies.hurt(*id, hit);
    for (s32 frame = 0; frame < 12; ++frame) {
        enemies.update(kTicks, kStep, {});
        CHECK(enemies.positionOf(*id).z <= 3 - 1 - enemies.radiusOf(*id) + 0.01f);
    }
}

TEST_CASE("enemies ride descending platforms while gameplay is frozen",
          "[game][enemies][enemy-platform][assets]") {
    test::FakeRenderDevice device;
    WorldCollision collision;
    const auto floor = triangle({-40, 0, -40}, {40, 0, -40}, {0, 0, 40}, {0, 1, 0});
    collision.build({floor});
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, Mat4{1});
    Enemies enemies;
    enemies.open(device, unpackedRoot(), &collision, 1, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.position = Vec3{2, 0, 0};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    const auto action = enemies.animatorOf(*id)->action();
    const auto stun = enemies.stunTicksOf(*id);
    const auto heading = enemies.memoryOf(*id).heading;
    for (s32 frame = 1; frame <= 60; ++frame) {
        const auto step = static_cast<f32>(frame);
        const Mat4 platform = glm::translate(Mat4{1}, Vec3{0, -0.5f * step, 0}) *
                              glm::rotate(Mat4{1}, 0.01f * step, Vec3{0, 1, 0});
        collision.setObjectTransform(0, platform);
        enemies.syncFloors();
        const Vec3 expected{platform * Vec4{spawn.position, 1}};
        CHECK(glm::distance(enemies.positionOf(*id), expected) < 0.001f);
        enemies.syncFloors(); // no accumulated delta on repeated synchronization
        CHECK(glm::distance(enemies.positionOf(*id), expected) < 0.001f);
        CHECK(enemies.animatorOf(*id)->action() == action);
        CHECK(enemies.stunTicksOf(*id) == stun);
        CHECK(enemies.memoryOf(*id).heading == heading);
    }
    // Ordinary updates carry bodies too, before AI; no movement input is needed.
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, -40, 0}));
    enemies.update(0, 0, {});
    CHECK(enemies.positionOf(*id).y == Approx(-40));
    const Vec3 before = enemies.positionOf(*id);
    collision.setSolid(0, false);
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, -60, 0}));
    enemies.syncFloors();
    CHECK(enemies.positionOf(*id) == before);
}

TEST_CASE("swarm sight measures collision centres across floors",
          "[game][enemies][enemy-sight][multiplayer][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 2, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id = enemies.spawn({.kind = kGruntKind, .tier = 1, .placed = true}, {});
    REQUIRE(id);
    auto elevated = playerAt({0, 40, 0}, 3);
    SECTION("another floor is outside the sight sphere") {
        enemies.update(kTicks, kStep, std::array{elevated});
        CHECK(enemies.targetOf(*id) == -1);
    }
    SECTION("a nearby floor wins over the horizontally closest player") {
        elevated.position.y = 20;
        const std::array players{elevated, playerAt({0, 0, 10}, 1)};
        enemies.update(kTicks, kStep, players);
        CHECK(enemies.targetOf(*id) == 1);
    }
    SECTION("native collision offset is not inferred from body height") {
        elevated.position = {0, 0, 0};
        elevated.collisionHeight = 23.0f;
        const std::array players{elevated, playerAt({0, 0, 10}, 1)};
        enemies.update(kTicks, kStep, players);
        CHECK(enemies.targetOf(*id) == 1);
    }
    SECTION("IT overrides range but losing the tag restores ordinary sight") {
        elevated.it = true;
        enemies.update(kTicks, kStep, std::array{elevated});
        CHECK(enemies.targetOf(*id) == 3);
        elevated.it = false;
        for (s32 frame = 0; frame < 8; ++frame) {
            enemies.update(kTicks, kStep, std::array{elevated});
        }
        CHECK(enemies.targetOf(*id) == -1);
    }
}

TEST_CASE("swarm target ties and crowding follow controller IDs rather than roster order",
          "[game][enemies][enemy-sight][multiplayer][assets]") {
    const bool reversed = GENERATE(false, true);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 2, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto first = enemies.spawn({.kind = kGruntKind, .tier = 1, .placed = true}, {});
    const auto second = enemies.spawn({.kind = kGruntKind, .tier = 1, .placed = true}, {});
    REQUIRE(first);
    REQUIRE(second);
    std::array players{playerAt({-20, 0, 0}, 1), playerAt({20, 0, 0}, 3)};
    if (reversed) {
        std::ranges::reverse(players);
    }
    enemies.update(kTicks, kStep, players);
    CHECK(enemies.targetOf(*first) == 1);
    CHECK(enemies.targetOf(*second) == 3);
}

TEST_CASE("fallen players do not block generator births but invisible standing players do",
          "[game][enemies][multiplayer][enemy-spawn][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 2, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const bool hidden = GENERATE(false, true);
    const bool invisible = GENERATE(false, true);
    auto blocker = playerAt({0, 0, 0}, 3);
    blocker.radius = 20; // covers every candidate octant
    blocker.hidden = hidden;
    blocker.invisible = invisible;
    const auto born =
        enemies.spawn({.kind = kGruntKind, .tier = 1, .generator = 7}, std::array{blocker});
    CHECK(born.has_value() == hidden);
}

TEST_CASE("swarm melee contacts the actual nearest live player independently of roster order",
          "[game][enemies][enemy-contact][multiplayer][assets]") {
    const bool reversed = GENERATE(false, true);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id = enemies.spawn({.kind = kGruntKind, .tier = 1, .placed = true}, {});
    REQUIRE(id);
    std::array players{playerAt({0, 0, 2}, 3), playerAt({0, 0, 0.5f}, 1)};
    s32 expected = 1;
    SECTION("the closer overlapping player receives the blow") {}
    SECTION("equal distances use the lower controller ID") {
        players[0].position.z = -0.5f;
    }
    SECTION("a fallen closer player has no contact body") {
        players[1].hidden = true;
        expected = 3;
    }
    SECTION("invisible bodies still intercept an enemy pursuing someone else") {
        players[1].invisible = true;
    }
    SECTION("the nearest collision centre can differ from the nearest feet") {
        players[1].collisionHeight = 9.0f;
        expected = 3;
    }
    if (reversed) {
        std::ranges::reverse(players);
    }
    std::vector<EnemyBlow> blows;
    const auto elapsed = stepsUntil(
        enemies, players,
        [&] {
            blows = enemies.takeBlows();
            return !blows.empty();
        },
        300);
    REQUIRE(elapsed < 300);
    REQUIRE(blows.size() == 1);
    CHECK(blows.front().player == expected);
}

TEST_CASE("a long enemy step cannot tunnel through a player's body",
          "[game][enemies][enemy-contact][body-contact][assets]") {
    const bool elevated = GENERATE(false, true);
    test::FakeRenderDevice device;
    EnemyScales scales;
    scales.speed = 100; // Twenty units per update: both endpoints are outside the body.
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id =
        enemies.spawn({.kind = kGruntKind, .tier = 1, .algorithm = kSeekWay, .placed = true}, {});
    REQUIRE(id);
    for (s32 frame = 0; frame < 120; ++frame) {
        enemies.update(kTicks, kStep, {});
    }
    REQUIRE(enemies.stunTicksOf(*id) <= 0);
    const Vec3 before = enemies.positionOf(*id);
    auto player = playerAt(before + Vec3{0, 0, 10}, 3);
    player.collisionHeight = elevated ? 20.0f : 3.0f;
    enemies.update(kTicks, kStep, std::array{player});
    REQUIRE(enemies.targetOf(*id) == 3);
    if (elevated) {
        CHECK(enemies.positionOf(*id).z == Approx(before.z + 20));
        CHECK_FALSE(enemies.animatorOf(*id)->swinging());
    } else {
        CHECK(enemies.positionOf(*id) == before);
        CHECK(enemies.animatorOf(*id)->swinging());
    }
}

TEST_CASE("enemy contact uses the authored collision centre instead of the player's feet",
          "[game][enemies][enemy-contact][body-contact][assets]") {
    const bool elevated = GENERATE(false, true);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id =
        enemies.spawn({.kind = kGruntKind, .tier = 1, .algorithm = kSeekWay, .placed = true}, {});
    REQUIRE(id);
    for (s32 frame = 0; frame < 120; ++frame) {
        enemies.update(kTicks, kStep, {});
    }
    const Vec3 before = enemies.positionOf(*id);
    auto player = playerAt(before + Vec3{0, 0, 2});
    player.collisionHeight = elevated ? 20.0f : 3.0f;
    enemies.update(kTicks, kStep, std::array{player});
    REQUIRE(enemies.targetOf(*id) == 0);
    CHECK(enemies.animatorOf(*id)->swinging() == !elevated);
    if (elevated) {
        CHECK(enemies.positionOf(*id).z > before.z);
    } else {
        CHECK(enemies.positionOf(*id) == before);
    }
}

TEST_CASE("world collision clips an enemy sweep before looking for player contact",
          "[game][enemies][enemy-contact][body-contact][assets]") {
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(yard());
    EnemyScales scales;
    scales.speed = 100;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), &collision, 1, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id = enemies.spawn({.kind = kGruntKind,
                                   .tier = 1,
                                   .algorithm = kSeekWay,
                                   .position = Vec3{0, 0, 10},
                                   .placed = true},
                                  {});
    REQUIRE(id);
    // Consume the initial stun and entry animation without taking a step.
    enemies.update(30, 2.0f, {});
    const Vec3 before = enemies.positionOf(*id);
    REQUIRE(before.z == Approx(10));
    const std::array party{playerAt({0, 0, 30})};
    enemies.update(kTicks, kStep, party);
    REQUIRE(enemies.targetOf(*id) == 0);
    CHECK(enemies.positionOf(*id).z > before.z);
    CHECK(enemies.positionOf(*id).z < 20); // the wall, not the player, stops the step
    CHECK_FALSE(enemies.animatorOf(*id)->swinging());
}

TEST_CASE("a knockback can carry an overlapping enemy away from a player",
          "[game][enemies][enemy-contact][body-contact][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id = enemies.spawn({.kind = kGruntKind, .tier = 3, .placed = true}, {});
    REQUIRE(id);
    const Vec3 before = enemies.positionOf(*id);
    EnemyHit knock;
    knock.damage = 2;
    knock.flags = EnemyHit::kKnockDown;
    knock.direction = {0, 0, 1};
    enemies.hurt(*id, knock);
    const std::array players{playerAt(before + Vec3{0, 0, -1})};
    enemies.update(kTicks, kStep, players);
    REQUIRE(enemies.targetOf(*id) == 0);
    CHECK(enemies.positionOf(*id).z > before.z);
    CHECK(enemies.takeBlows().empty());
}

TEST_CASE("swarm contact does not expose an invisible party without a sight target",
          "[game][enemies][enemy-contact][multiplayer][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto id = enemies.spawn({.kind = kGruntKind, .tier = 1, .placed = true}, {});
    REQUIRE(id);
    auto player = playerAt({0, 0, 0.5f}, 3);
    player.invisible = true;
    for (s32 frame = 0; frame < 120; ++frame) {
        enemies.update(kTicks, kStep, std::array{player});
        CHECK(enemies.targetOf(*id) == -1);
        CHECK(enemies.takeBlows().empty());
    }
}

TEST_CASE("swarm movement treats sleeping bodies as solid and dying bodies as clear",
          "[game][enemies][enemy-crowd][assets]") {
    const bool dying = GENERATE(false, true);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 2, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto walker =
        enemies.spawn({.kind = kGruntKind, .tier = 1, .algorithm = kSeekWay, .placed = true}, {});
    REQUIRE(walker);
    // Finish entry/stun before placing either the target or the body in its path.
    for (s32 frame = 0; frame < 120; ++frame) {
        enemies.update(kTicks, kStep, {});
    }
    const Vec3 before = enemies.positionOf(*walker);
    const std::array players{playerAt(before + Vec3{0, 0, 20})};
    const f32 clearance = 2 * enemies.radiusOf(*walker);
    const Vec3 blocking = before + Vec3{0, 0, clearance + 0.01f};
    const auto blocker = enemies.spawn(
        {.kind = kGruntKind, .tier = 1, .position = blocking, .placed = true, .asleep = true}, {});
    REQUIRE(blocker);
    if (dying) {
        EnemyHit hit;
        hit.damage = 1000;
        enemies.hurt(*blocker, hit);
        REQUIRE(enemies.dying(*blocker));
    }
    enemies.update(kTicks, kStep, players);
    const Vec3 after = enemies.positionOf(*walker);
    if (dying) {
        // The walker updates before the corpse's animation; the body is still present
        // when both the path probe and final movement collision inspect it.
        CHECK(after.z > before.z + 0.01f);
        CHECK(after.x == Approx(before.x).margin(0.001f));
        CHECK(glm::distance(after, blocking) < clearance);
    } else {
        CHECK(glm::distance(after, blocking) >= clearance);
    }
}

TEST_CASE("a dying enemy releases spawn clearance before its body disappears",
          "[game][enemies][enemy-spawn][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 2, {}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto blocker = enemies.spawn({.kind = kGruntKind, .tier = 1, .placed = true}, {});
    REQUIRE(blocker);
    const EnemySpawn birth{.kind = kGruntKind, .tier = 1, .generator = 7};
    CHECK_FALSE(enemies.spawn(birth, {}));
    EnemyHit hit;
    hit.damage = 1000;
    enemies.hurt(*blocker, hit);
    REQUIRE(enemies.dying(*blocker));
    // The spawn sweep (fn_8004646C) skips DYING, not merely INACTIVE.
    CHECK(enemies.spawn(birth, {}).has_value());
    CHECK(enemies.dying(*blocker));
}

TEST_CASE("generator births cannot cross a wall to an unobstructed destination",
          "[game][enemies][enemy-collision][assets]") {
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(yard());
    Enemies enemies;
    const bool itemWall = GENERATE(false, true);
    enemies.open(device, unpackedRoot(), itemWall ? nullptr : &collision, 1, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.position = Vec3{0, 0, 16};
    spawn.clearance = 12;
    const std::array boxes{
        Obstacle{.centre = Vec3{0, 0, 20}, .halfAcross = 40, .halfAlong = 0.25f, .height = 8}};
    const auto id =
        enemies.spawn(spawn, {}, itemWall ? std::span{boxes} : std::span<const Obstacle>{});
    REQUIRE_FALSE(id.has_value());
}

TEST_CASE("generator birth walls are tested before settling onto an upper landing",
          "[game][enemies][enemy-collision][alpha-spawn-height][assets]") {
    // check_enemy_pos (8004F9AC) rejects the start-to-offset WallCollide before
    // FloorCollide can change the birth height. A valid upper floor must not
    // lift that wall test over an obstruction on the path out of the generator.
    constexpr s32 kLeaderWay = 12;
    bool wall = true;
    f32 birthY = 1;
    SECTION("low wall blocks the route beneath the upper landing") {}
    SECTION("an unobstructed route may settle onto the upper landing") {
        wall = false;
    }
    SECTION("a birth already above the low wall remains clear") {
        birthY = 6;
    }
    std::vector<CollisionTriangle> geometry{
        triangle({-20, 6, 5}, {20, 6, 5}, {20, 6, 20}, {0, 1, 0}),
        triangle({-20, 6, 5}, {20, 6, 20}, {-20, 6, 20}, {0, 1, 0})};
    if (wall) {
        geometry.push_back(triangle({-20, 0, 4}, {20, 3, 4}, {20, 0, 4}, {0, 0, -1}));
        geometry.push_back(triangle({-20, 0, 4}, {-20, 3, 4}, {20, 3, 4}, {0, 0, -1}));
    }
    WorldCollision collision;
    collision.build(geometry);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), &collision, 1, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.algorithm = kLeaderWay; // One forward exit isolates the wall/floor query order.
    spawn.generator = 0;
    spawn.position = {0, birthY, 0};
    spawn.clearance = 8;
    const f32 radius = enemyKind(kGruntKind).radius;
    const f32 height = enemyKind(kGruntKind).height;
    const Vec3 candidate{0, birthY, spawn.clearance + radius};
    const auto floor = collision.floorAt(candidate, 6, 6);
    REQUIRE(floor);
    REQUIRE(floor->y == Approx(6));
    const Vec3 landing{candidate.x, floor->y, candidate.z};
    // Both the final destination and the incorrectly elevated path are clear.
    REQUIRE(collision.resolveWalls(landing, radius, 6.1f, 6 + height - 0.1f) == landing);
    REQUIRE(collision.sweepWalls(spawn.position, landing, radius, 6.1f, 6 + height - 0.1f) ==
            landing);
    const bool blocked = wall && birthY < 6;
    const Vec3 swept = collision.sweepWalls(spawn.position, candidate, radius, birthY + 0.1f,
                                            birthY + height - 0.1f);
    CHECK((swept.z < candidate.z - 0.01f) == blocked);
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value() == !blocked);
    if (id) {
        CHECK(enemies.positionOf(*id) == landing);
    }
    const auto events = enemies.takeGeneratorEvents();
    REQUIRE(events.size() == (blocked ? 0 : 1));
    if (!blocked) {
        CHECK(events[0].generator == 0);
        CHECK(events[0].kind == EnemyGeneratorEvent::Kind::Born);
    }
    enemies.close();
}

TEST_CASE("a newly released Death cannot be pushed through a wall by an item body",
          "[game][enemies][enemy-collision][assets]") {
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(yard());
    Enemies enemies;
    enemies.open(device, unpackedRoot(), &collision, 1, {}, 7);
    REQUIRE(enemies.loadKind(kDeathKind));
    EnemySpawn spawn;
    spawn.kind = kDeathKind;
    spawn.placed = true;
    spawn.position = Vec3{0, 0, 17};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    const std::array boxes{
        Obstacle{.centre = Vec3{0, 0, 15}, .halfAcross = 10, .halfAlong = 6, .height = 8}};
    enemies.update(kTicks, kStep, {}, boxes);
    CHECK(enemies.positionOf(*id).z < 20);
}

TEST_CASE("Temple enemies cannot walk through the closed front doors",
          "[game][enemies][temple-doors][assets]") {
    const auto root = unpackedRoot();
    test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2");
    test::assetOrSkip("WDATA/TEMPLE.WAD");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    REQUIRE(world.load(device, root, *level));
    Enemies enemies;
    enemies.open(device, root, &world.collision(), 1, {}, 7);
    REQUIRE(enemies.loadKind(23));
    EnemySpawn spawn;
    spawn.kind = 23;
    spawn.placed = true;
    spawn.algorithm = 7;
    spawn.position = Vec3{2, 0, 78};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    const std::array party{playerAt(Vec3{2, 0, 94})};
    SECTION("knockback also checks the path through the door") {
        for (s32 frame = 0; frame < 16; ++frame) {
            enemies.update(kTicks, kStep, {});
        }
        EnemyHit hit;
        hit.damage = 1;
        hit.flags = EnemyHit::kKnockDown;
        enemies.hurt(*id, hit);
        enemies.update(30, 0.5f, {});
        CHECK(enemies.positionOf(*id).z < 85);
    }
    SECTION("ordinary pursuit stays on its side") {
        for (s32 frame = 0; frame < 300; ++frame) {
            enemies.update(kTicks, kStep, party);
            REQUIRE(enemies.positionOf(*id).z < 85);
        }
    }
}

TEST_CASE("battlefield archers keep aiming and shooting at close players while retreating",
          "[game][enemies][battlefield-archer][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 7);
    constexpr s32 kBattlefieldKind = 19;
    REQUIRE(enemies.loadKind(kBattlefieldKind));
    EnemySpawn spawn;
    spawn.kind = kBattlefieldKind;
    spawn.tier = kArcherStrength;
    spawn.algorithm = kSkirmishWay;
    spawn.placed = true;
    spawn.direction = Vec3{0, 0, -1};
    spawn.throwInterval = 0.2f;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    EnemyMissiles missiles;
    for (s32 frame = 0; frame < 120; ++frame) {
        // Stay close on the opposite side of its initial facing, outside body contact.
        const std::array party{playerAt(enemies.positionOf(*id) + Vec3{0, 0, 8})};
        enemies.update(kTicks, kStep, party, {}, &missiles);
    }
    CHECK(std::cos(enemies.yawOf(*id)) > 0.99f);
    CHECK(missiles.count() > 0);
}

TEST_CASE("the battlefield entrance archers remain on their authored perches and fire nearby",
          "[game][enemies][battlefield-archer][assets]") {
    const auto* level = GENERATE("LEVELH1", "LEVELH3");
    const auto root = unpackedRoot();
    const auto directory = root / "LEVELS" / level;
    test::assetOrSkip(std::string{"LEVELS/"} + level + "/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    WorldCollision collision;
    REQUIRE(collision.load(directory, layout));
    const Vec3 perch = std::string_view{level} == "LEVELH1" ? Vec3{56.875f, 20.125f, 41.5f}
                                                            : Vec3{17.25f, 6.484375f, 58.125f};
    const auto placed = std::ranges::find(layout.itemInstances(), perch, &ItemInstance::position);
    REQUIRE(placed != layout.itemInstances().end());
    REQUIRE(placed->params[0] == kArcherStrength);
    REQUIRE(placed->params[2] == kThrowWay);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, &collision, 1, {}, 7);
    REQUIRE(enemies.loadKind(19));
    EnemySpawn spawn;
    spawn.kind = 19;
    spawn.tier = kArcherStrength;
    spawn.algorithm = placed->params[2];
    spawn.position = perch;
    spawn.direction = Vec3{std::sin(placed->rotation.y), 0, std::cos(placed->rotation.y)};
    spawn.placed = true;
    spawn.throwInterval = 0.2f;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    EnemyMissiles missiles;
    const Vec3 initial = enemies.positionOf(*id);
    const Vec3 player = initial - spawn.direction * 8.0f;
    const std::array party{playerAt(player)};
    for (s32 frame = 0; frame < 300; ++frame) {
        enemies.update(kTicks, kStep, party, {}, &missiles);
        REQUIRE(enemies.positionOf(*id) == initial);
    }
    CHECK(missiles.count() > 0);
}

TEST_CASE("a grunt is bred ahead of its generator, chases the player it sees and strikes on touch",
          "[game][enemies][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    EnemyScales scales;
    scales.health = 0.75f;
    enemies.open(device, unpackedRoot(), nullptr, 13, scales, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    REQUIRE(enemies.kindLoaded(kGruntKind));
    REQUIRE(enemies.treeOf(kGruntKind, 2) != nullptr);
    REQUIRE(enemies.treeOf(kGruntKind, 2)->name == "GRU2");
    REQUIRE(enemies.paceOf(kGruntKind) == Approx(0.1f));
    // Born a body's width past the generator's clearance, ahead of it or ahead to a side.
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.tier = 2;
    spawn.algorithm = 7;
    spawn.position = Vec3{0.0f, 0.0f, 0.0f};
    spawn.direction = Vec3{0.0f, 0.0f, 1.0f};
    spawn.clearance = 5.0f;
    spawn.generator = 3;
    const std::vector<EnemyView> nobody;
    const auto id = enemies.spawn(spawn, nobody);
    REQUIRE(id.has_value());
    REQUIRE(enemies.count() == 1);
    REQUIRE(enemies.alive(*id));
    REQUIRE(enemies.kindOf(*id) == kGruntKind);
    REQUIRE(enemies.tierOf(*id) == 2);
    REQUIRE(enemies.generatorOf(*id) == 3);
    REQUIRE(enemies.algorithmOf(*id) == 7);
    REQUIRE(enemies.positionOf(*id).z > 4.0f);
    REQUIRE(std::abs(enemies.positionOf(*id).x) <= enemies.positionOf(*id).z + 0.01f);
    REQUIRE(glm::length(enemies.positionOf(*id)) == Approx(6.5f).margin(0.01f));
    // Two tiers of a grunt's thirty, at the level's three quarters.
    REQUIRE(enemies.healthOf(*id) == Approx(30.0f * 0.333f * 2.0f * 0.75f));
    REQUIRE(enemies.radiusOf(*id) == 1.5f);
    REQUIRE(enemies.heightOf(*id) == 6.0f);
    REQUIRE(enemies.animatorOf(*id) != nullptr);
    REQUIRE(enemies.animatorOf(*id)->entering());
    // Nobody about: it walks in and wanders, seeing no one.
    for (s32 i = 0; i < 30; ++i) {
        enemies.update(kTicks, kStep, nobody);
    }
    REQUIRE(enemies.targetOf(*id) < 0);
    REQUIRE(enemies.animatorOf(*id)->moving());
    // A player in sight is chased: it closes in, facing the player, until it is against them.
    const std::vector<EnemyView> party{playerAt(Vec3{20.0f, 0.0f, 20.0f})};
    const Vec3 from = enemies.positionOf(*id);
    enemies.update(kTicks, kStep, party);
    REQUIRE(enemies.targetOf(*id) == 0);
    const s32 closing = stepsUntil(
        enemies, party,
        [&] { return glm::distance(enemies.positionOf(*id), party[0].position) < 3.5f; }, 600);
    REQUIRE(closing < 600);
    REQUIRE(glm::distance(enemies.positionOf(*id), party[0].position) <
            glm::distance(from, party[0].position));
    // Against the player it swings; the blow lands as the swing ends, on the player it is
    // for: two thirds of the grunt's fifteen, its second tier's health being just under
    // two thirds of its kind's, as the original has it.
    std::vector<EnemyBlow> blows;
    const s32 swinging = stepsUntil(
        enemies, party,
        [&] {
            auto taken = enemies.takeBlows();
            blows.insert(blows.end(), taken.begin(), taken.end());
            return !blows.empty();
        },
        300);
    REQUIRE(swinging < 300);
    REQUIRE(blows.size() == 1);
    REQUIRE(blows[0].player == 0);
    REQUIRE(blows[0].kind == kGruntKind);
    REQUIRE(blows[0].damage == Approx(15.0f * 0.667f));
    REQUIRE_FALSE(blows[0].power);
    REQUIRE((blows[0].direction.z > 0.0f || blows[0].direction.x > 0.0f));
    // It keeps at it; every eighth blow is the power one, half as strong again and, from a
    // tall body, knocking the player down.
    for (s32 i = 0; i < 2000 && blows.size() < 8; ++i) {
        enemies.update(kTicks, kStep, party);
        auto taken = enemies.takeBlows();
        blows.insert(blows.end(), taken.begin(), taken.end());
    }
    REQUIRE(blows.size() >= 8);
    REQUIRE(blows[7].power);
    REQUIRE((blows[7].flags & EnemyHit::kKnockBack) != 0);
    REQUIRE(blows[7].damage == Approx(15.0f * 0.667f * 1.5f));
    REQUIRE_FALSE(blows[6].power);
    // A blow does not land on someone who has gone.
    const std::vector<EnemyView> hidden{[] {
        EnemyView view = playerAt(Vec3{20.0f, 0.0f, 20.0f});
        view.hidden = true;
        return view;
    }()};
    for (s32 i = 0; i < 120; ++i) {
        enemies.update(kTicks, kStep, hidden);
    }
    REQUIRE(enemies.takeBlows().empty());
    REQUIRE(enemies.targetOf(*id) < 0);
}

TEST_CASE("first-tier spear grunts land repeated blows at point-blank contact",
          "[game][enemies][spear-contact][assets]") {
    const s32 kind = GENERATE(kGruntKind, 5, 10, 13, 19);
    const f32 distance = GENERATE(0.0f, 0.5f, 2.0f);
    const s32 ticks = GENERATE(1, 2);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 7);
    REQUIRE(enemies.loadKind(kind));
    EnemySpawn spawn;
    spawn.kind = kind;
    spawn.tier = 1;
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    const std::array party{playerAt(enemies.positionOf(*id) + Vec3{0, 0, distance})};
    s32 blows = 0;
    for (s32 frame = 0; frame < 600 / ticks; ++frame) {
        enemies.update(ticks, static_cast<f32>(ticks) / 60.0f, party);
        blows += static_cast<s32>(enemies.takeBlows().size());
    }
    CAPTURE(kind, distance, ticks, enemies.animatorOf(*id)->action());
    CHECK(blows >= 8);
}

TEST_CASE("Treasury first-tier grunts damage a level 99 Knight at actual body separation",
          "[enemies][spear-contact][treasury-contact][assets]") {
    const s32 ticks = GENERATE(1, 2);
    const f32 gap = GENERATE(-1.0f, 0.0f, 0.4f);
    const f32 armorBonus = GENERATE(0.0f, 150.0f);
    const auto root = unpackedRoot();
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    const auto* stats = classes.stats(5);
    REQUIRE(stats);
    CharacterSave save;
    save.character = 5;
    save.color = 2;
    save.progress().experience = levelExperience(99);
    save.progress().armorAdd = armorBonus;
    save.progress().health = 9999;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, save, stats, {}, 0);
    test::FakeRenderDevice device;
    Enemies enemies;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("A4");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.level());
    const s32 kind = levelKindOf(world.level()->enemies, kGruntKind, 1);
    const f32 separation = players[0].actor.radius() + enemyKind(kind).radius + gap;
    players[0].actor.spawn(0, save, stats, {0, 0, separation}, 0);
    const auto party = LevelOpponents::enemyViews(players);
    REQUIRE(party[0].level == 99);
    EnemyScales scales;
    scales.damage = world.level()->tuning.enemyDamage;
    enemies.open(device, root, nullptr, 1, scales, 7);
    REQUIRE(enemies.loadKind(kind));
    EnemySpawn spawn;
    spawn.kind = kind;
    spawn.tier = 1;
    spawn.placed = true;
    REQUIRE(enemies.spawn(spawn, {}).has_value());
    PlayerHealth health;
    PlayerHealth::Events events;
    events.sound = [](std::string_view) {};
    events.cry = [](std::string_view) {};
    events.named = [](std::string_view, f32) {};
    s32 blows = 0;
    f32 ordinaryDamage = 0;
    s32 ordinaryHealthLoss = 0;
    for (s32 tick = 0; tick < 600; tick += ticks) {
        enemies.update(ticks, static_cast<f32>(ticks) / 60, party);
        for (const auto& blow : enemies.takeBlows()) {
            ++blows;
            const s32 before = players[0].actor.save().health();
            if (!blow.power) {
                ordinaryDamage = blow.damage;
            }
            health.hurt(players[0], blow.damage, HurtKind::Blow, true, false,
                        world.level()->tuning.damage, events, {blow.flags, blow.direction}, false,
                        stats);
            if (!blow.power) {
                ordinaryHealthLoss += before - players[0].actor.save().health();
            }
        }
    }
    CAPTURE(kind, ticks, gap, separation, party[0].height, party[0].collisionHeight, blows,
            armorBonus, ordinaryDamage, world.level()->tuning.damage,
            armorDefense(*stats, save.progress()));
    CHECK(blows >= 8);
    CHECK(players[0].actor.save().health() < save.health());
    CHECK(ordinaryDamage == Approx(4.74525f));
    if (armorBonus > 0) {
        // Max-stat armor absorbs this tier's normal stab in retail, not its eighth
        // stronger swing. Contact is still registered; enlarging the spear is wrong.
        CHECK(ordinaryDamage < armorDefense(*stats, save.progress()));
        CHECK(ordinaryHealthLoss == 0);
    } else {
        // 4.74525 damage - 4.25 armor must add up across normal stabs, even though
        // every individual hit would incorrectly round to zero on its own.
        CHECK(ordinaryHealthLoss > 0);
    }
}

TEST_CASE("a grunt struck flinches, thrown down gets up, and killed is worth its experience",
          "[game][enemies][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 13, EnemyScales{}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.tier = 3;
    spawn.placed = true;
    spawn.position = Vec3{0.0f, 0.0f, 0.0f};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    REQUIRE((enemies.positionOf(*id) == Vec3{0.0f, 0.0f, 0.0f}));
    REQUIRE(enemies.healthOf(*id) == Approx(29.97f));
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 25.0f})};
    for (s32 i = 0; i < 30; ++i) {
        enemies.update(kTicks, kStep, party);
    }
    // A hit is worth two, its damage under a point counting as one, and a flinch.
    EnemyHit hit;
    hit.damage = 0.5f;
    hit.player = 0;
    hit.direction = Vec3{0.0f, 0.0f, -1.0f};
    enemies.hurt(*id, hit);
    REQUIRE(enemies.healthOf(*id) == Approx(28.97f));
    auto losses = enemies.takeLosses();
    REQUIRE(losses.size() == 1);
    REQUIRE(losses[0].enemy == *id);
    REQUIRE(losses[0].player == 0);
    REQUIRE(losses[0].experience == 2);
    REQUIRE_FALSE(losses[0].killed);
    enemies.update(kTicks, kStep, party);
    REQUIRE(enemies.animatorOf(*id)->action() == EnemyAction::HitReact1);
    REQUIRE(enemies.pushCountOf(*id) == 0);
    // A knocking hit throws it back the way the hit went, and down.
    EnemyHit knock;
    knock.damage = 4.0f;
    knock.flags = EnemyHit::kKnockDown;
    knock.player = 0;
    knock.direction = Vec3{0.0f, 0.0f, -1.0f};
    for (s32 i = 0; i < 20; ++i) {
        enemies.update(kTicks, kStep, party);
    }
    const Vec3 before = enemies.positionOf(*id);
    enemies.hurt(*id, knock);
    REQUIRE(enemies.takeLosses().size() == 1);
    enemies.update(kTicks, kStep, party);
    REQUIRE(enemies.animatorOf(*id)->action() == EnemyAction::HitReact2);
    REQUIRE(enemies.pushCountOf(*id) == 1);
    // Thrown down, it slides a fifth less each 30 Hz frame than the one before (do_enemies).
    std::vector<f32> slides;
    for (s32 i = 0; i < 10; ++i) {
        const f32 z = enemies.positionOf(*id).z;
        enemies.update(kTicks, kStep, party);
        slides.push_back(z - enemies.positionOf(*id).z);
    }
    for (usize i = 1; i < 4; ++i) {
        CHECK(slides[i + 1] / slides[i] == Approx(0.8f).margin(0.001f));
    }
    REQUIRE(enemies.positionOf(*id).z < before.z - 0.5f);
    // A character over the level the place is meant for hits a tenth harder a level.
    EnemyScales scales;
    scales.playerLevel = 10.0f;
    Enemies seasoned;
    seasoned.open(device, unpackedRoot(), nullptr, 13, scales, 1);
    REQUIRE(seasoned.loadKind(kGruntKind));
    const auto other = seasoned.spawn(spawn, {});
    REQUIRE(other.has_value());
    EnemyHit strong;
    strong.damage = 10.0f;
    strong.player = 0;
    strong.level = 20;
    seasoned.hurt(*other, strong);
    REQUIRE(seasoned.healthOf(*other) == Approx(29.97f - 20.0f));
    EnemyHit weak = strong;
    weak.level = 5;
    seasoned.hurt(*other, weak);
    REQUIRE(seasoned.healthOf(*other) == Approx(29.97f - 20.0f - 9.5f));
    // The killing blow is worth four; the body plays out its fall and is gone.
    EnemyHit slay;
    slay.damage = 100.0f;
    slay.player = 0;
    enemies.hurt(*id, slay);
    losses = enemies.takeLosses();
    REQUIRE(losses.size() == 1);
    REQUIRE(losses[0].killed);
    REQUIRE(losses[0].experience == 4);
    REQUIRE(losses[0].kind == kGruntKind);
    REQUIRE(losses[0].tier == 3);
    enemies.update(kTicks, kStep, party);
    REQUIRE(enemies.dying(*id));
    REQUIRE_FALSE(enemies.alive(*id));
    REQUIRE(enemies.targets().empty()); // no longer to be shot
    enemies.hurt(*id, slay);            // nor hurt
    REQUIRE(enemies.takeLosses().empty());
    const s32 falling = stepsUntil(enemies, party, [&] { return enemies.count() == 0; }, 200);
    REQUIRE(falling < 200);
    REQUIRE(falling > 3);
}

TEST_CASE("a grunt gets round a wall between it and its player, stops at a ledge, and does "
          "not walk through another",
          "[game][enemies][assets]") {
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(yard());
    Enemies enemies;
    enemies.open(device, unpackedRoot(), &collision, 13, EnemyScales{}, 11);
    REQUIRE(enemies.loadKind(kGruntKind));
    // The wall is between them, the player off to one side: straight at the player runs
    // into it.
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.tier = 1;
    spawn.algorithm = kChaseWay;
    spawn.placed = true;
    spawn.position = Vec3{0.0f, 0.0f, 10.0f};
    const std::vector<EnemyView> party{playerAt(Vec3{8.0f, 0.0f, 30.0f})};
    const auto id = enemies.spawn(spawn, party);
    REQUIRE(id.has_value());
    bool bumped = false;
    f32 furthestAside = 0.0f;
    const s32 steps = stepsUntil(
        enemies, party,
        [&] {
            furthestAside = std::max(furthestAside, std::abs(enemies.positionOf(*id).x));
            bumped =
                bumped || (enemies.positionOf(*id).z > 17.0f && enemies.positionOf(*id).z < 20.0f);
            return enemies.positionOf(*id).z > 21.0f;
        },
        2400);
    REQUIRE(steps < 2400);
    REQUIRE(bumped);                // it did run into the wall
    REQUIRE(furthestAside > 12.0f); // and went round its end
    REQUIRE(stepsUntil(
                enemies, party,
                [&] {
                    return enemies.targetOf(*id) == 0 &&
                           glm::distance(enemies.positionOf(*id), party[0].position) < 4.0f;
                },
                1200) < 1200);
    // Beyond x thirty there is no floor: a player over the edge is not followed off it.
    const std::vector<EnemyView> beyond{playerAt(Vec3{36.0f, 0.0f, 30.0f})};
    for (s32 i = 0; i < 900; ++i) {
        enemies.update(kTicks, kStep, beyond);
    }
    REQUIRE(enemies.positionOf(*id).x <= 30.5f);
    REQUIRE(enemies.positionOf(*id).x > 24.0f);
    // Two placed in a line to a player: the one behind is stopped by the one in front.
    Enemies queue;
    queue.open(device, unpackedRoot(), &collision, 13, EnemyScales{}, 2);
    REQUIRE(queue.loadKind(kGruntKind));
    spawn.position = Vec3{-30.0f, 0.0f, -20.0f};
    const auto front = queue.spawn(spawn, {});
    spawn.position = Vec3{-30.0f, 0.0f, -26.0f};
    const auto behind = queue.spawn(spawn, {});
    REQUIRE(front.has_value());
    REQUIRE(behind.has_value());
    const std::vector<EnemyView> north{playerAt(Vec3{-30.0f, 0.0f, 0.0f})};
    for (s32 i = 0; i < 60; ++i) {
        queue.update(kTicks, kStep, north);
        REQUIRE(glm::distance(queue.positionOf(*front), queue.positionOf(*behind)) >= 2.9f);
    }
    // A knocked one carries half its push onto the one it is thrown against: one standing
    // still behind the one struck is shoved back.
    Enemies pair;
    pair.open(device, unpackedRoot(), &collision, 13, EnemyScales{}, 2);
    REQUIRE(pair.loadKind(kGruntKind));
    spawn.position = Vec3{-30.0f, 0.0f, -3.0f};
    spawn.algorithm = kLoiterWay; // a generator's loiterer turns on the spot
    spawn.generator = 0;
    const auto struck = pair.spawn(spawn, {});
    spawn.position = Vec3{-30.0f, 0.0f, -6.5f};
    const auto shoved = pair.spawn(spawn, {});
    REQUIRE(struck.has_value());
    REQUIRE(shoved.has_value());
    for (s32 i = 0; i < 30; ++i) {
        pair.update(kTicks, kStep, north);
    }
    REQUIRE(pair.positionOf(*shoved).z == Approx(-6.5f));
    EnemyHit knock;
    knock.damage = 2.0f;
    knock.flags = EnemyHit::kKnockDown;
    knock.direction = Vec3{0.0f, 0.0f, -1.0f};
    knock.player = 0;
    pair.hurt(*struck, knock);
    for (s32 i = 0; i < 20; ++i) {
        pair.update(kTicks, kStep, north);
    }
    REQUIRE(pair.positionOf(*shoved).z < -6.6f);
    REQUIRE(pair.positionOf(*struck).z < -3.5f);
}

TEST_CASE("the swarm is found by missiles, sweeps and strikes, is capped, and sleeps until woken",
          "[game][enemies][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 2, EnemyScales{}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    spawn.position = Vec3{0.0f, 0.0f, 10.0f};
    const auto first = enemies.spawn(spawn, {});
    spawn.position = Vec3{10.0f, 0.0f, 0.0f};
    spawn.asleep = true;
    const auto second = enemies.spawn(spawn, {});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    // Missiles see both bodies; a sweep finds the nearest it touches; a burst and an arc
    // find those within them.
    const auto targets = enemies.targets();
    REQUIRE(targets.size() == 2);
    REQUIRE(targets[0].radius == 1.5f);
    REQUIRE(targets[0].height == 6.0f);
    REQUIRE((enemies.struckBy(Vec3{0.0f, 3.0f, -5.0f}, Vec3{0.0f, 3.0f, 30.0f}, 0.5f) == first));
    REQUIRE((enemies.struckBy(Vec3{-5.0f, 3.0f, 0.0f}, Vec3{30.0f, 3.0f, 0.0f}, 0.5f) == second));
    REQUIRE_FALSE(
        enemies.struckBy(Vec3{-5.0f, 3.0f, -5.0f}, Vec3{-5.0f, 3.0f, 30.0f}, 0.5f).has_value());
    REQUIRE(enemies.within(Vec3{0.0f, 3.0f, 0.0f}, 12.0f).size() == 2);
    REQUIRE(enemies.within(Vec3{0.0f, 3.0f, 0.0f}, 5.0f).empty());
    REQUIRE((enemies.reachedBy(Vec3{0.0f, 0.0f, 0.0f}, 12.0f, 0.5f, Vec3{0.0f, 0.0f, 1.0f}) ==
             std::vector<s32>{*first}));
    REQUIRE(enemies.reachedBy(Vec3{0.0f, 0.0f, 0.0f}, 12.0f, 3.2f, Vec3{0.0f, 0.0f, 1.0f}).size() ==
            2);
    // The sleeper does not stir for a player; woken, it does.
    const std::vector<EnemyView> party{playerAt(Vec3{10.0f, 0.0f, 20.0f})};
    for (s32 i = 0; i < 30; ++i) {
        enemies.update(kTicks, kStep, party);
    }
    REQUIRE((enemies.positionOf(*second) == Vec3{10.0f, 0.0f, 0.0f}));
    REQUIRE(enemies.targetOf(*first) == 0);
    enemies.wake(*second);
    for (s32 i = 0; i < 60; ++i) {
        enemies.update(kTicks, kStep, party);
    }
    REQUIRE((enemies.positionOf(*second) != Vec3{10.0f, 0.0f, 0.0f}));
    // The level's cap holds: a third takes the slot of the one least worth keeping, the
    // one furthest off, when the request permits replacing a visible enemy.
    spawn.asleep = false;
    spawn.position = Vec3{-10.0f, 0.0f, -10.0f};
    spawn.priority = EnemySpawn::Priority::Visible;
    const auto third = enemies.spawn(spawn, party);
    REQUIRE(third.has_value());
    REQUIRE(enemies.count() == 2);
    spawn.tier = 1;
    Enemies strong;
    strong.open(device, unpackedRoot(), nullptr, 1, EnemyScales{}, 1);
    REQUIRE(strong.loadKind(kGruntKind));
    EnemySpawn great = spawn;
    great.tier = 3;
    REQUIRE(strong.spawn(great, {}).has_value());
    spawn.priority = EnemySpawn::Priority::FreeSlotOnly;
    REQUIRE_FALSE(strong.spawn(spawn, {}).has_value());
    spawn.priority = EnemySpawn::Priority::Offscreen;
    // On screen, as the last update found it, an ordinary birth may not take its place.
    ViewVolume watching;
    watching.position = strong.positionOf(0) - Vec3{0, 0, 20};
    strong.setView(watching);
    strong.update(kTicks, kStep, {});
    CHECK(strong.inView() == 1); // do_enemies' visible count, for the turbo lesson
    REQUIRE_FALSE(strong.spawn(spawn, {}).has_value());
    REQUIRE(strong.tierOf(0) == 3);
    ViewVolume away = watching;
    away.forward = Vec3{0, 0, -1};
    strong.setView(away);
    strong.update(kTicks, kStep, {});
    CHECK(strong.inView() == 0);
    REQUIRE(strong.spawn(spawn, {}).has_value()); // strength is not replacement importance
    REQUIRE(strong.tierOf(0) == 1);
    REQUIRE(strong.spawn(great, {}).has_value());
    // A generator gone is forgotten by what it bred.
    great.generator = 5;
    Enemies bred;
    bred.open(device, unpackedRoot(), nullptr, 5, EnemyScales{}, 1);
    REQUIRE(bred.loadKind(kGruntKind));
    const auto child = bred.spawn(great, {});
    REQUIRE(child.has_value());
    REQUIRE(bred.generatorOf(*child) == 5);
    bred.generatorGone(5);
    REQUIRE(bred.generatorOf(*child) == -1);
    // Rats are given their own way whatever they are asked, turning on a player near them.
    REQUIRE(bred.loadKind(kRatKind));
    EnemySpawn rat;
    rat.kind = kRatKind;
    rat.algorithm = 7;
    rat.placed = true;
    rat.position = Vec3{0.0f, 0.0f, 0.0f};
    const auto vermin = bred.spawn(rat, {});
    REQUIRE(vermin.has_value());
    const s32 prowl = bred.algorithmOf(*vermin);
    REQUIRE((prowl == kProwlWay || prowl == kMirroredProwlWay));
    REQUIRE(enemyMindOf(prowl).name() == "prowl");
    // A player near it is gone for while near, the prowl kept all the while (move_logic02).
    const std::vector<EnemyView> near{playerAt(Vec3{0.0f, 0.0f, 5.0f})};
    for (s32 i = 0; i < 20; ++i) {
        bred.update(kTicks, kStep, near);
    }
    CHECK(bred.algorithmOf(*vermin) == prowl);
}

TEST_CASE("swarm elemental hits use arena multipliers but retain their minimum damage",
          "[enemies][damage][assets]") {
    const auto root = unpackedRoot();
    test::FakeRenderDevice device;
    for (const bool bossEncounter : {false, true}) {
        CAPTURE(bossEncounter);
        Enemies enemies;
        enemies.open(device, root, nullptr, 4, EnemyScales{.bossEncounter = bossEncounter}, 7);
        REQUIRE(enemies.loadKind(kGruntKind));
        const auto id =
            enemies.spawn(EnemySpawn{.kind = kGruntKind, .tier = 3, .placed = true}, {});
        REQUIRE(id);
        const f32 before = enemies.healthOf(*id);
        EnemyHit hit;
        hit.damage = 10;
        hit.flags = 1;
        hit.player = 0;
        enemies.hurt(*id, hit);
        const f32 expected = (10 - enemyKind(kGruntKind).armor) * (bossEncounter ? 1.25f : 1.5f);
        CHECK(enemies.healthOf(*id) == Approx(before - expected));
        REQUIRE(enemies.alive(*id));
        const f32 after = enemies.healthOf(*id);
        hit.damage = 0.1f;
        hit.flags = 0;
        enemies.hurt(*id, hit);
        CHECK(enemies.healthOf(*id) == Approx(after - 1));
    }
}

TEST_CASE("enemy hits queue feedback once and animate masked death skins to completion",
          "[game][enemies][enemy-feedback][assets]") {
    const auto root = unpackedRoot();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.tier = 2;
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EnemyHit hit;
    hit.damage = 1;
    hit.player = 0;
    hit.close = true;
    enemies.hurt(*id, hit);
    auto feedback = enemies.takeFeedback();
    REQUIRE(feedback.size() == 1);
    const std::array roster{LevelEnemy{kGruntKind, 2, "GRUNT"}};
    CHECK(feedback[0].sound(roster) == "S_GRUNT2HIT1CLO");
    enemies.draw(device, Mat4{1}, {}, &device.whiteTexture(), &weapons);
    REQUIRE_FALSE(device.draws.empty());
    CHECK(std::ranges::any_of(device.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == &device.whiteTexture();
    }));
    enemies.update(kTicks, kStep, {});
    CHECK(enemies.animatorOf(*id)->action() == EnemyAction::HitReact1);
    hit.damage = 1000;
    enemies.hurt(*id, hit);
    enemies.hurt(*id, hit);
    CHECK_FALSE(enemies.alive(*id));
    CHECK(enemies.targets().empty());
    feedback = enemies.takeFeedback();
    REQUIRE(feedback.size() == 1);
    CHECK(feedback[0].killed);
    CHECK(feedback[0].sound(roster) == "S_GRUNT2DIECLOS");
    device.draws.clear();
    enemies.draw(device, Mat4{1}, {}, &device.whiteTexture(), &weapons);
    REQUIRE_FALSE(device.draws.empty());
    const auto skin = weapons.textures.find("DTH_BLOOD00");
    REQUIRE(skin);
    CHECK(std::ranges::any_of(device.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == &weapons.textures.texture(device, *skin);
    }));
    for (s32 frame = 0; frame < 3; ++frame) {
        enemies.update(kTicks, kStep, {});
    }
    device.draws.clear();
    enemies.draw(device, Mat4{1}, {}, &device.whiteTexture(), &weapons);
    CHECK(std::ranges::any_of(device.draws, [&](const auto& draw) {
        return draw.state.maskedTexture == &weapons.textures.texture(device, *skin + 1);
    }));
    // Every remaining death draw must retain the dissolve skin. In particular,
    // reaching its tenth frame must never restore the original body texture.
    for (s32 frame = 0; frame < 30; ++frame) {
        enemies.update(kTicks, kStep, {});
        device.draws.clear();
        enemies.draw(device, Mat4{1}, {}, &device.whiteTexture(), &weapons);
        for (const auto& draw : device.draws) {
            CHECK(draw.state.maskedTexture != nullptr);
        }
    }
    CHECK(enemies.count() == 0);
    CHECK(enemies.takeFeedback().empty());
    const auto again = enemies.spawn(spawn, {});
    REQUIRE(again);
    hit.damage = 1;
    enemies.hurt(*again, hit);
    feedback = enemies.takeFeedback();
    REQUIRE(feedback.size() == 1);
    CHECK(feedback[0].hitCount == 1);
    enemies.hurt(*again, hit);
    enemies.close();
    CHECK(enemies.takeFeedback().empty());
}

TEST_CASE("small enemies retire on their first death update without an invented animation hold",
          "[game][enemies][enemy-feedback][death-retirement]") {
    const f32 step = GENERATE(1.0f / 60, 1.0f / 30, 1.0f / 15);
    const auto root = test::scratchDirectory("enemy-death-retirement");
    const auto dir = root / "MONSTERS/HAN";
    std::filesystem::create_directories(dir);
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(dir);
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"HAN1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":30,"rate":30}]}]})");
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 1, {}, 1);
    REQUIRE(enemies.loadKind(22));
    const auto id = enemies.spawn({.kind = 22, .placed = true}, {});
    REQUIRE(id);
    enemies.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    EnemyHit hit;
    hit.damage = 1000;
    hit.player = 0;
    enemies.hurt(*id, hit);
    const auto feedback = enemies.takeFeedback();
    REQUIRE(feedback.size() == 1);
    REQUIRE(feedback[0].killed);
    REQUIRE(feedback[0].deathSkin().empty());
    const auto losses = enemies.takeLosses();
    REQUIRE(losses.size() == 1);
    CHECK(losses[0].killed);
    // do_enemies' DYING case removes a body when skinfx.nframes <= 0,
    // even when READY is the only animation available. The burst is a separate effect.
    enemies.update(static_cast<s32>(std::lround(step * 60)), step, {});
    CHECK(enemies.count() == 0);
    device.draws.clear();
    enemies.draw(device, Mat4{1}, {});
    CHECK(device.draws.empty());
    enemies.update(kTicks, kStep, {});
    CHECK(enemies.takeLosses().empty());
    CHECK(enemies.takeFeedback().empty());
}

TEST_CASE("Dream World hands vanish promptly while imps and warlocks play their death skins",
          "[game][enemies][enemy-feedback][death-retirement][assets]") {
    const auto root = unpackedRoot();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    test::FakeRenderDevice device;
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    for (const s32 kind : {22, 23, 24}) {
        for (const s32 tier : {1, 2, 3}) {
            CAPTURE(kind, tier);
            Enemies enemies;
            enemies.open(device, root, nullptr, 1, {}, 1);
            REQUIRE(enemies.loadKind(kind));
            const auto id = enemies.spawn({.kind = kind, .tier = tier, .placed = true}, {});
            REQUIRE(id);
            EnemyHit hit;
            hit.damage = 1000;
            hit.player = 0;
            enemies.hurt(*id, hit);
            enemies.update(kTicks, kStep, {});
            if (kind == 22) {
                CHECK(enemies.count() == 0);
            } else {
                REQUIRE(enemies.count() == 1);
                REQUIRE(enemies.animatorOf(*id)->dying());
                device.draws.clear();
                enemies.draw(device, Mat4{1}, {}, nullptr, &weapons);
                REQUIRE_FALSE(device.draws.empty());
                for (const auto& draw : device.draws) {
                    CHECK(draw.state.maskedTexture != nullptr);
                }
            }
            for (s32 frame = 0; frame < 22; ++frame) {
                enemies.update(kTicks, kStep, {});
            }
            CHECK(enemies.count() == 0);
            device.draws.clear();
            enemies.draw(device, Mat4{1}, {}, nullptr, &weapons);
            CHECK(device.draws.empty());
        }
    }
}
TEST_CASE("Mikey attracts the swarm to the dropped point without becoming a damageable body",
          "[mikey][enemies][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 1, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    spawn.algorithm = 0;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    // The real body is beyond sight, but its dropped Mikey is within sight to the right.
    std::array party{playerAt({0, 0, 100})};
    party[0].decoy = Vec3{10, 3, 0};
    for (s32 frame = 0; frame < 90; ++frame) {
        enemies.update(kTicks, kStep, party);
    }
    REQUIRE(enemies.targetOf(*id) == 0);
    CHECK(enemies.positionOf(*id).x > 1);
    CHECK(std::abs(enemies.positionOf(*id).z) < 1);
    CHECK(enemies.takeBlows().empty());
    CHECK(enemies.animatorOf(*id)->action() != EnemyAction::Attack);
    party[0].decoy.reset();
    for (s32 frame = 0; frame < 10; ++frame) {
        enemies.update(kTicks, kStep, party);
    }
    CHECK(enemies.targetOf(*id) == -1);
}

TEST_CASE("invisibility breaks swarm targeting without removing the physical player",
          "[game][items][enemies][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 4, {}, 7);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    std::vector<EnemyView> party{playerAt({0, 0, 10})};
    enemies.update(kTicks, kStep, party);
    REQUIRE(enemies.targetOf(*id) == 0);
    party[0].invisible = true;
    enemies.update(kTicks, kStep, party);
    CHECK(enemies.targetOf(*id) == -1);
    CHECK_FALSE(party[0].hidden);
    party[0].invisible = false;
    enemies.update(kTicks, kStep, party);
    CHECK(enemies.targetOf(*id) == 0);
}
std::filesystem::path routingAssets() {
    const auto root = test::scratchDirectory("enemy-routing");
    const auto archive = root / "MONSTERS/GRU";
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"GRU1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":10,"rate":30},
                     {"name":"WALK","frames":10,"rate":30}]}]})");
    return root;
}

TEST_CASE("enemy texture clocks interpolate once per stock and stop with the simulation",
          "[game][enemies][cadence][texture-animation]") {
    const auto root = routingAssets();
    writeTextFile(root / "MONSTERS/GRU/animations.json", R"({"trees":[{"name":"GRU1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":10,"rate":30}]}],
        "textureAnimations":[{"name":"SKIN","texture":0,"source":-3,
                              "frames":60,"rate":2,"flag":-1}]})");
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.algorithm = kLurkWay;
    spawn.placed = true;
    REQUIRE(enemies.spawn(spawn, {}).has_value());
    spawn.position.x = 10;
    REQUIRE(enemies.spawn(spawn, {}).has_value());
    enemies.update(1, 1.0f / 60, {});
    const auto offsetAt = [&](f32 alpha) {
        device.draws.clear();
        enemies.draw(device, Mat4{1}, {}, nullptr, nullptr, nullptr, alpha);
        REQUIRE(device.draws.size() == 2);
        CHECK(device.draws[0].state.uvOffset == device.draws[1].state.uvOffset);
        return device.draws[0].state.uvOffset.y;
    };
    CHECK(offsetAt(0) == Approx(0));
    CHECK(offsetAt(0.5f) == Approx(1.0f / 480));
    CHECK(offsetAt(1) == Approx(1.0f / 240));
    CHECK(offsetAt(0.5f) == Approx(1.0f / 480));
    enemies.update(1, 1.0f / 60, {}, {}, nullptr, 1, true);
    CHECK(offsetAt(0) == Approx(1.0f / 240));
    CHECK(offsetAt(1) == Approx(1.0f / 240));
}

/** An acid blob of a stance and the two hit reactions. */
std::filesystem::path blobAssets() {
    const auto root = test::scratchDirectory("enemy-blob");
    const auto archive = root / "MONSTERS/ACI";
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"ACI1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":10,"rate":30},
                     {"name":"HIT1","frames":6,"rate":30},
                     {"name":"HIT2","frames":12,"rate":30}]}]})");
    return root;
}

TEST_CASE("a placement sees as far as it says at the level's scale, else thirty, and Death "
          "without end",
          "[game][enemies][enemy-sight]") {
    // SetItem (items.c 5566-5569) against init_enemy_vars' thirty times the level's visrad.
    test::FakeRenderDevice device;
    Enemies enemies;
    EnemyScales scales;
    scales.sight = 1.5f;
    enemies.open(device, routingAssets(), nullptr, 6, scales, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn told;
    told.algorithm = kLurkWay;
    told.placed = true;
    told.sight = 12.0f;
    EnemySpawn untold = told;
    untold.sight = 0.0f;
    untold.position = Vec3{10.0f, 0.0f, 0.0f};
    const auto near = enemies.spawn(told, {});
    const auto far = enemies.spawn(untold, {});
    REQUIRE(near.has_value());
    REQUIRE(far.has_value());
    CHECK(enemies.sightOf(*near) == Approx(18.0f));
    CHECK(enemies.sightOf(*far) == Approx(45.0f));
    // A lurker wakes only for a player within its sight: twenty off, the short-sighted one
    // stands where it was placed while the other comes.
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 20.0f})};
    for (s32 i = 0; i < 120; ++i) {
        enemies.update(kTicks, kStep, party);
    }
    CHECK(enemies.positionOf(*near) == told.position);
    CHECK(enemies.positionOf(*far) != untold.position);
    CHECK(enemies.animatorOf(*near)->action() == EnemyAction::Ready);
    CHECK(enemies.animatorOf(*far)->action() != EnemyAction::Ready);
    // What a generator breeds is given no sight of its own.
    EnemySpawn bred;
    bred.generator = 0;
    bred.position = Vec3{-20.0f, 0.0f, 0.0f};
    const auto born = enemies.spawn(bred, {});
    REQUIRE(born.has_value());
    CHECK(enemies.sightOf(*born) == Approx(45.0f));
}

TEST_CASE("a placement of ordinary strength stands still thirty ticks on appearing; the "
          "variants, a sleeper and what generators breed do not",
          "[game][enemies][enemy-stun]") {
    // SetItem, items.c 5560-5564: stun_timer 30 under strength four, unless it sleeps first.
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, routingAssets(), nullptr, 10, {}, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto placedAt = [&](s32 tier, f32 x, bool asleep = false) {
        EnemySpawn spawn;
        spawn.tier = tier;
        spawn.algorithm = kChaseWay;
        spawn.placed = true;
        spawn.asleep = asleep;
        spawn.position = Vec3{x, 0.0f, 0.0f};
        const auto id = enemies.spawn(spawn, {});
        REQUIRE(id.has_value());
        return *id;
    };
    const s32 ordinary = placedAt(2, 0.0f);
    const s32 strongest = placedAt(3, 5.0f);
    const s32 archer = placedAt(kArcherStrength, 10.0f);
    const s32 suicide = placedAt(kSuicideStrength, 15.0f);
    const s32 sleeper = placedAt(1, 20.0f, true);
    EnemySpawn bred;
    bred.tier = 1;
    bred.algorithm = kChaseWay;
    bred.generator = 0;
    bred.position = Vec3{-10.0f, 0.0f, 0.0f};
    const auto born = enemies.spawn(bred, {});
    REQUIRE(born.has_value());
    CHECK(enemies.stunTicksOf(ordinary) == Enemies::kPlacedStun);
    CHECK(enemies.stunTicksOf(strongest) == Enemies::kPlacedStun);
    CHECK(enemies.stunTicksOf(archer) == 0);
    CHECK(enemies.stunTicksOf(suicide) == 0);
    CHECK(enemies.stunTicksOf(sleeper) == 0);
    CHECK(enemies.stunTicksOf(*born) == 0);
    enemies.wake(sleeper);
    CHECK(enemies.stunTicksOf(sleeper) == 0);
    // Through its thirty ticks the stunned one stays put with a player before it, and the
    // bred one is already on its way.
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 20.0f})};
    const Vec3 start = enemies.positionOf(ordinary);
    const Vec3 bornAt = enemies.positionOf(*born);
    for (s32 i = 0; i < Enemies::kPlacedStun / kTicks; ++i) {
        enemies.update(kTicks, kStep, party);
        CHECK(enemies.positionOf(ordinary) == start);
    }
    CHECK(enemies.stunTicksOf(ordinary) == 0);
    CHECK(enemies.positionOf(*born) != bornAt);
    enemies.update(kTicks, kStep, party);
    CHECK(enemies.positionOf(ordinary) != start);
}

TEST_CASE("the acid blob is rooted: a knock-back leaves it as it was and a floor hit throws it "
          "down where it stands",
          "[game][enemies][enemy-rooted]") {
    // fn_8004DC2C's E_ACID cases (enemy.c 5573-5610): no flinch or push for a knock-back, a
    // knock-down with a push of nought.
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, blobAssets(), nullptr, 4, {}, 3);
    constexpr s32 kAcidKind = 21;
    REQUIRE(enemies.loadKind(kAcidKind));
    EnemySpawn spawn;
    spawn.kind = kAcidKind;
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 20.0f})};
    enemies.update(kTicks, kStep, party);
    EnemyHit knock;
    knock.damage = 1.0f;
    knock.flags = EnemyHit::kKnockBack;
    knock.player = 0;
    knock.direction = Vec3{0.0f, 0.0f, -1.0f};
    enemies.hurt(*id, knock);
    enemies.update(kTicks, kStep, party);
    CHECK(enemies.animatorOf(*id)->action() == EnemyAction::Ready);
    CHECK(enemies.pushCountOf(*id) == 0);
    CHECK(enemies.healthOf(*id) < enemyKind(kAcidKind).healthAtTier(1));
    EnemyHit floor = knock;
    floor.flags = EnemyHit::kKnockDown;
    enemies.hurt(*id, floor);
    enemies.update(kTicks, kStep, party);
    CHECK(enemies.animatorOf(*id)->action() == EnemyAction::HitReact2);
    CHECK(enemies.pushCountOf(*id) == 0);
    for (s32 i = 0; i < 10; ++i) {
        enemies.update(kTicks, kStep, party);
        CHECK(enemies.positionOf(*id) == spawn.position);
    }
}

TEST_CASE("the garm brood sends its death shot from its corpse at its player as the body goes",
          "[game][enemies][assets][death-shot]") {
    // kill_enemy's fn_8004F1DC: toward its target, else the first standing player; nobody
    // standing, nothing.
    test::assetOrSkip("MONSTERS/GRM/ANIM.PS2");
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 4, {}, 5);
    REQUIRE(enemies.loadKind(kGarmBroodKind));
    EnemySpawn spawn;
    spawn.kind = kGarmBroodKind;
    spawn.tier = 3;
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 25.0f}, 2)};
    for (s32 i = 0; i < 10; ++i) {
        enemies.update(kTicks, kStep, party);
    }
    EnemyHit slay;
    slay.damage = 1000.0f;
    slay.player = 2;
    enemies.hurt(*id, slay);
    enemies.update(kTicks, kStep, party);
    REQUIRE(enemies.dying(*id));
    CHECK(enemies.takeDeathShots().empty()); // not before the body is gone
    const Vec3 lay = enemies.positionOf(*id);
    REQUIRE(stepsUntil(enemies, party, [&] { return enemies.count() == 0; }, 300) < 300);
    const auto shots = enemies.takeDeathShots();
    REQUIRE(shots.size() == 1);
    CHECK(shots[0].enemy == *id);
    CHECK(shots[0].kind == kGarmBroodKind);
    CHECK(glm::distance(shots[0].position, lay) < 1.0f);
    const Vec3 way = glm::normalize(party[0].position - shots[0].position);
    CHECK(glm::distance(shots[0].direction, way) < 0.01f);
    CHECK(enemies.takeDeathShots().empty());
    // With nobody standing there is nothing to aim at.
    const auto again = enemies.spawn(spawn, {});
    REQUIRE(again.has_value());
    enemies.update(kTicks, kStep, party);
    enemies.hurt(*again, slay);
    std::vector<EnemyView> fallen = party;
    fallen[0].hidden = true;
    REQUIRE(stepsUntil(enemies, fallen, [&] { return enemies.count() == 0; }, 300) < 300);
    CHECK(enemies.takeDeathShots().empty());
}

TEST_CASE("the warlock comes and goes: seen a while, faded out, unseen a while, back again",
          "[game][enemies][veil]") {
    const auto root = test::scratchDirectory("enemy-veil");
    const auto archive = root / "MONSTERS/WAR";
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"WAR1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":10,"rate":30},
                     {"name":"WALK","frames":10,"rate":30}]}]})");
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 2, {}, 7);
    REQUIRE(enemies.loadKind(Enemies::kVeilingKind));
    const auto warlock =
        enemies.spawn(EnemySpawn{.kind = Enemies::kVeilingKind, .tier = 1, .placed = true}, {});
    REQUIRE(warlock.has_value());
    CHECK(enemies.opacityOf(*warlock) == 1.0f);
    // Seen for sixty to a hundred and nineteen ticks once it goes about (after its entrance),
    // then fading out sixteen a tick.
    s32 seen = 0;
    while (enemies.opacityOf(*warlock) == 1.0f && seen < 600) {
        enemies.update(kTicks, kStep, {});
        seen += kTicks;
    }
    CHECK(seen >= 60);
    CHECK(seen < 600);
    s32 fading = 0;
    while (enemies.opacityOf(*warlock) > 0.0f && fading < 400) {
        enemies.update(kTicks, kStep, {});
        fading += kTicks;
    }
    CHECK(fading <= 18);
    // Gone from sight, body and shadow, for a while, then back.
    device.draws.clear();
    ItemArchive weapons;
    enemies.draw(device, Mat4{1}, {}, &device.whiteTexture(), &weapons);
    enemies.drawShadows(device, Mat4{1}, kFarEye, {});
    CHECK(device.draws.empty());
    s32 unseen = 0;
    while (enemies.opacityOf(*warlock) == 0.0f && unseen < 400) {
        enemies.update(kTicks, kStep, {});
        unseen += kTicks;
    }
    CHECK(unseen >= 40);
    CHECK(unseen < 400);
    // Any kind but the warlock stays seen.
    REQUIRE(enemies.loadKind(kGruntKind) == false); // no archive: nothing to veil
}

TEST_CASE("a swarm body lies the shadow of its tier under it", "[game][enemies][shadow]") {
    const auto root = routingAssets();
    const auto archive = root / "MONSTERS/GRU";
    writeTextFile(archive / "flat.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 0 1\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1},
        {"index":1,"name":"SHADOW2L1","file":"flat.obj","meshTriangles":1}]})");
    test::convertModelFixture(archive);
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto shadows = [&] {
        device.draws.clear();
        enemies.drawShadows(device, Mat4{1}, kFarEye, {});
        return device.draws.size();
    };
    // The first tier's is missing from this archive: none, and nothing breaks.
    const auto first = enemies.spawn(
        EnemySpawn{.kind = kGruntKind, .tier = 1, .position = Vec3{-9, 0, 0}, .placed = true}, {});
    REQUIRE(first);
    CHECK(shadows() == 0);
    const auto second = enemies.spawn(
        EnemySpawn{.kind = kGruntKind, .tier = 2, .position = Vec3{5, 2, 3}, .placed = true}, {});
    REQUIRE(second);
    REQUIRE(shadows() == 1);
    CHECK(glm::distance(device.draws[0].vertices[0].position,
                        Vec3{5, 2 + BlobShadow::kLift + BlobShadow::kPull, 3}) < 0.001f);
    // The bodies' own draws leave it out: it goes after the level's floors.
    device.draws.clear();
    ItemArchive weapons;
    enemies.draw(device, Mat4{1}, {}, &device.whiteTexture(), &weapons);
    CHECK(
        std::ranges::none_of(device.draws, [](const auto& draw) { return !draw.state.cullBack; }));
}

TEST_CASE("the zombies' own archive lies a shadow under each tier",
          "[game][enemies][shadow][assets]") {
    const auto root = unpackedRoot();
    test::assetOrSkip("MONSTERS/ZOM/ANIM.PS2");
    constexpr s32 kZombieKind = 13;
    REQUIRE(enemyKind(kZombieKind).name == std::string_view{"ZOM"});
    test::FakeRenderDevice device;
    for (s32 tier = 1; tier <= 3; ++tier) {
        CAPTURE(tier);
        Enemies enemies;
        enemies.open(device, root, nullptr, 4, {}, 7);
        REQUIRE(enemies.loadKind(kZombieKind));
        const auto id = enemies.spawn(
            EnemySpawn{.kind = kZombieKind, .tier = tier, .position = Vec3{0}, .placed = true}, {});
        REQUIRE(id);
        const auto shadows = [&] {
            device.draws.clear();
            enemies.drawShadows(device, Mat4{1}, kFarEye, {});
            return std::ranges::count_if(device.draws, [](const auto& draw) {
                return !draw.state.depthWrite && !draw.state.cullBack;
            });
        };
        // None while it rises (action 1, START), then one.
        REQUIRE(enemies.animatorOf(*id)->action() == EnemyAction::Start);
        CHECK(shadows() == 0);
        for (s32 frame = 0; frame < 300 && enemies.animatorOf(*id)->action() == EnemyAction::Start;
             ++frame) {
            enemies.update(kTicks, kStep, {});
        }
        REQUIRE(enemies.animatorOf(*id)->action() != EnemyAction::Start);
        CHECK(shadows() == 1);
    }
}

TEST_CASE("off screen the swarm waits unless its player is in sight, and never strikes",
          "[game][enemies][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 4, {}, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.algorithm = kChaseWay;
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    ViewVolume away;
    away.position = Vec3{0, 0, -40};
    away.forward = Vec3{0, 0, -1}; // looking away from it, well back
    enemies.setView(away);
    // Nobody in sight and off screen: it stands just as it is.
    const std::array far{playerAt(Vec3{0, 0, 100})};
    for (s32 frame = 0; frame < 60; ++frame) {
        enemies.update(kTicks, kStep, far);
    }
    CHECK(enemies.positionOf(*id) == Vec3{0});
    // A player within its sight: it goes for them though off screen, but never lays a blow.
    const std::array close{playerAt(Vec3{0, 0, 3})};
    for (s32 frame = 0; frame < 120; ++frame) {
        enemies.update(kTicks, kStep, close);
    }
    CHECK(enemies.positionOf(*id).z > 0.0f);
    CHECK(enemies.takeBlows().empty());
    // On screen, against them, it strikes.
    ViewVolume watching;
    watching.position = Vec3{0, 0, -20};
    enemies.setView(watching);
    bool struck = false;
    for (s32 frame = 0; frame < 240 && !struck; ++frame) {
        enemies.update(kTicks, kStep, close);
        struck = !enemies.takeBlows().empty();
    }
    CHECK(struck);
}

TEST_CASE("a sentry paces between its lookouts", "[game][enemies][mind]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, routingAssets(), nullptr, 4, {}, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    std::vector<WorldLocator> locators(2);
    locators[0].kind = LocatorKind::Sentry;
    locators[0].position = Vec3{0, 0, 10};
    locators[0].next = 1;
    locators[1].kind = LocatorKind::Sentry;
    locators[1].position = Vec3{0, 0, -10};
    locators[1].next = 0;
    enemies.setLookouts(LookoutRoute::of(locators));
    EnemySpawn spawn;
    spawn.algorithm = kPatrolWay;
    spawn.placed = true;
    spawn.position = Vec3{0, 0, 2};
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    f32 north = 0.0f;
    f32 south = 0.0f;
    for (s32 frame = 0; frame < 900; ++frame) {
        enemies.update(kTicks, kStep, {});
        north = std::max(north, enemies.positionOf(*id).z);
        south = std::min(south, enemies.positionOf(*id).z);
    }
    CHECK(north > 9.0f);
    CHECK(south < -9.0f);
}

TEST_CASE("zig-zaggers react to the collision from this movement step without waiting a frame",
          "[game][enemies][zigzag-step]") {
    // move_logic14 calls do_enemy_move before testing dead_end and re-aiming.
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, routingAssets(), nullptr, 1, {}, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.algorithm = kZigZagWay;
    spawn.zigZagSide = 1;
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id);
    const std::array party{playerAt({0, 0, 25})};
    for (s32 step = 0; step < 20; ++step) {
        enemies.update(kTicks, kStep, party);
    }
    const auto before = enemies.memoryOf(*id);
    REQUIRE(before.zigZag.count > kTicks);
    REQUIRE(before.zigZag.hold <= 0);
    REQUIRE_FALSE(enemies.blockedOf(*id));
    const Vec3 from = enemies.positionOf(*id);
    const Vec3 direction{std::sin(before.heading), 0, std::cos(before.heading)};
    constexpr f32 kBlockerRadius = 1;
    const f32 separation = enemies.radiusOf(*id) + kBlockerRadius;
    const f32 halfStep = enemies.paceOf(kGruntKind) * static_cast<f32>(kTicks) * 0.5f;
    const std::array bodies{
        MissileTarget{0, from + direction * (separation + halfStep), kBlockerRadius, 8}};
    enemies.setCombatantBodies(bodies);
    enemies.update(kTicks, kStep, party);
    REQUIRE(enemies.blockedOf(*id));
    CHECK(enemies.positionOf(*id) == from);
    const auto& after = enemies.memoryOf(*id);
    CHECK(after.zigZag.count == 0);
    CHECK(after.zigZag.swings == 0);
    CHECK(after.zigZag.spread == before.zigZag.spread + 1);
    CHECK(after.zigZag.hold == 30);
    const f32 offset = std::numbers::pi_v<f32> / 4 +
                       std::numbers::pi_v<f32> / 12 * static_cast<f32>(before.zigZag.spread);
    const Vec3 toPlayer = party[0].position - from;
    const f32 facing = std::atan2(toPlayer.x, toPlayer.z);
    CHECK(after.heading == Approx(wrapAngle(facing + (before.zigZag.side > 0 ? offset : -offset))));
}

TEST_CASE("chasers route around generator bodies instead of pushing into them forever",
          "[game][enemies][enemy-routing]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, routingAssets(), nullptr, 4, {}, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.algorithm = kChaseWay;
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    const std::array party{playerAt(Vec3{0, 0, 22})};
    const std::array obstacles{
        Obstacle{.centre = Vec3{0, 0, 9}, .halfAcross = 4, .halfAlong = 3, .height = 8}};
    for (s32 frame = 0; frame < 900; ++frame) {
        enemies.update(kTicks, kStep, party, obstacles);
        CHECK(glm::distance(obstacles[0].pushOut(enemies.positionOf(*id), enemies.radiusOf(*id)),
                            enemies.positionOf(*id)) < 0.001f);
    }
    CHECK(glm::distance(enemies.positionOf(*id), party[0].position) < 4);
}

TEST_CASE("chasers pass a stationary enemy and can recover from an existing overlap",
          "[game][enemies][enemy-routing]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, routingAssets(), nullptr, 4, {}, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.algorithm = kLoiterWay; // a generator's loiterer turns on the spot
    spawn.generator = 0;
    spawn.placed = true;
    spawn.position = Vec3{0, 0, 7};
    const auto front = enemies.spawn(spawn, {});
    REQUIRE(front.has_value());
    spawn.algorithm = kChaseWay;
    spawn.generator = -1;
    spawn.position = Vec3{0, 0, 0};
    SECTION("separate bodies") {}
    SECTION("overlapping placement") {
        spawn.position.z = 5;
    }
    SECTION("coincident placement") {
        spawn.position.z = 7;
    }
    const auto behind = enemies.spawn(spawn, {});
    REQUIRE(behind.has_value());
    const std::array party{playerAt(Vec3{0, 0, 22})};
    const f32 separation = enemies.radiusOf(*front) + enemies.radiusOf(*behind);
    for (s32 frame = 0; frame < 900; ++frame) {
        const f32 before = glm::distance(enemies.positionOf(*front), enemies.positionOf(*behind));
        enemies.update(kTicks, kStep, party);
        const f32 after = glm::distance(enemies.positionOf(*front), enemies.positionOf(*behind));
        CHECK(after + 0.001f >= std::min(before, separation));
    }
    CHECK(glm::distance(enemies.positionOf(*behind), party[0].position) < 4);
}
TEST_CASE("an item that cancels a walking step reports a blocked body to its mind",
          "[game][enemies][enemy-routing]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, routingAssets(), nullptr, 4, {}, 3);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    // Prowl attempts the straight step. Wander now probes and avoids this item
    // before movement, so it cannot exercise the body's cancelled-step feedback.
    spawn.algorithm = kProwlWay;
    spawn.placed = true;
    spawn.position.z = 0.5f;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    const std::array obstacles{
        Obstacle{.centre = Vec3{0, 0, 3}, .halfAcross = 4, .halfAlong = 1, .height = 8}};
    // A placement stands still its first thirty ticks, touching nothing.
    while (enemies.stunTicksOf(*id) > 0) {
        enemies.update(kTicks, kStep, {}, obstacles);
        CHECK_FALSE(enemies.blockedOf(*id));
    }
    enemies.update(kTicks, kStep, {}, obstacles);
    CHECK(enemies.positionOf(*id) == spawn.position);
    CHECK(enemies.bumpedWallOf(*id));
    CHECK(enemies.blockedOf(*id));
}

TEST_CASE("an enemy on a burning floor has a quarter-second repeat gate independent of render rate",
          "[game][enemies][hazards][assets]") {
    const auto dir = test::scratchDirectory("enemy-hazard-floor");
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "EMBERS", "position": [0, 0, 0], "flags": 65540, "next": -1,
               "child": -1}],
  "animations": [], "particles": [], "locators": [], "itemInfos": [], "itemInstances": []
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    HazardSurfaces hazards;
    hazards.bind(layout);
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const std::vector<CollisionTriangle> floor{
        triangle({-40, 0, -40}, {40, 0, 40}, {40, 0, -40}, up),
        triangle({-40, 0, -40}, {-40, 0, 40}, {40, 0, 40}, up)};
    WorldCollision collision;
    collision.build(floor);
    test::FakeRenderDevice device;
    std::optional<f32> nativeHealth;
    for (const s32 frequency : {30, 60, 144}) {
        CAPTURE(frequency);
        Enemies enemies;
        enemies.open(device, unpackedRoot(), &collision, 13, EnemyScales{}, 1);
        enemies.setHazards(&hazards);
        REQUIRE(enemies.loadKind(kGruntKind));
        EnemySpawn spawn;
        spawn.kind = kGruntKind;
        spawn.tier = 3;
        spawn.placed = true;
        const auto id = enemies.spawn(spawn, {});
        REQUIRE(id.has_value());
        const f32 whole = enemies.healthOf(*id);
        s32 elapsed = 0;
        for (s32 call = 1; elapsed < 4; ++call) {
            const s32 next = std::min(4, call * 60 / frequency);
            enemies.update(next - elapsed, static_cast<f32>(next - elapsed) / 60, {});
            elapsed = next;
        }
        REQUIRE(enemies.healthOf(*id) < whole);
        if (!nativeHealth) {
            nativeHealth = enemies.healthOf(*id);
        }
        CHECK(enemies.healthOf(*id) == Approx(*nativeHealth));
        const f32 firstHit = whole - enemies.healthOf(*id);
        const f32 afterFirst = enemies.healthOf(*id);
        enemies.update(12, 0.2f, {});
        CHECK(enemies.healthOf(*id) == Approx(afterFirst));
        enemies.update(2, 1.0f / 30, {});
        CHECK(enemies.healthOf(*id) == Approx(afterFirst - firstHit));
        CHECK(enemies.takeLosses().empty()); // the world's harm is worth nothing to anyone
        enemies.close();
    }
}

TEST_CASE("swarm decisions movement and random holds are independent of update batching",
          "[game][enemies][enemy-cadence][assets]") {
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(yard());
    struct Snapshot {
        Vec3 position;
        f32 yaw;
        MindMemory memory;
        s32 target;
        EnemyAction action;
    };
    std::vector<Snapshot> native;
    for (const s32 frequency : {30, 60, 144}) {
        CAPTURE(frequency);
        Enemies enemies;
        enemies.open(device, unpackedRoot(), &collision, 71, {}, 3);
        REQUIRE(enemies.loadKind(kGruntKind));
        const std::array party{playerAt(Vec3{0, 0, 28}, 0), playerAt(Vec3{16, 0, 30}, 2)};
        std::vector<s32> ids;
        for (const s32 algorithm : {kChaseWay, kCastWay, kSkirmishWay}) {
            EnemySpawn spawn;
            spawn.kind = kGruntKind;
            spawn.placed = true;
            spawn.algorithm = algorithm;
            spawn.position = Vec3{static_cast<f32>(ids.size()) * 5 - 5, 0, 10};
            const auto id = enemies.spawn(spawn, party);
            REQUIRE(id);
            ids.push_back(*id);
        }
        s32 elapsed = 0;
        for (s32 call = 1; call <= 4 * frequency; ++call) {
            const s32 next = call * 60 / frequency;
            // The application supplies completed simulation time, not render
            // time (a 144 Hz render can complete no simulation ticks at all).
            enemies.update(next - elapsed, static_cast<f32>(next - elapsed) / 60, party);
            elapsed = next;
        }
        for (usize i = 0; i < ids.size(); ++i) {
            const s32 id = ids[i];
            const Snapshot actual{enemies.positionOf(id), enemies.yawOf(id), enemies.memoryOf(id),
                                  enemies.targetOf(id), enemies.animatorOf(id)->action()};
            if (frequency == 30) {
                native.push_back(actual);
                continue;
            }
            const auto& expected = native[i];
            CHECK(glm::distance(actual.position, expected.position) == Approx(0).margin(0.00001f));
            CHECK(actual.yaw == Approx(expected.yaw));
            CHECK(actual.memory.heading == Approx(expected.memory.heading));
            CHECK(actual.memory.stuck == expected.memory.stuck);
            CHECK(actual.memory.counter == expected.memory.counter);
            CHECK(actual.memory.fuse == expected.memory.fuse);
            CHECK(actual.memory.deadEnd == expected.memory.deadEnd);
            CHECK(actual.target == expected.target);
            CHECK(actual.action == expected.action);
        }
    }
}

TEST_CASE("the swarm runs from a lit suicide bomber near it", "[game][enemies][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 13, EnemyScales{}, 2);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.tier = kSuicideStrength;
    spawn.algorithm = 0; // unset: the strength's own
    spawn.position = Vec3{-30.0f, 0.0f, 0.0f};
    const auto bomber = enemies.spawn(spawn, {});
    REQUIRE(bomber.has_value());
    REQUIRE(enemies.algorithmOf(*bomber) == kSuicideWay);
    spawn.tier = 1;
    spawn.algorithm = kChaseWay;
    spawn.position = Vec3{-24.0f, 0.0f, 0.0f};
    spawn.placed = false; // FoundSuicideBomber excludes placed enemies (birth_style != 0).
    const auto chaser = enemies.spawn(spawn, {});
    REQUIRE(chaser.has_value());
    const std::vector<EnemyView> near{playerAt(Vec3{-30.0f, 0.0f, 12.0f})};
    // Until the fuse is lit the chaser goes for the player like any other.
    s32 frame = 0;
    for (; frame < 600 && enemies.animatorOf(*bomber)->action() != EnemyAction::Run; ++frame) {
        enemies.update(kTicks, kStep, near);
    }
    REQUIRE(enemies.animatorOf(*bomber)->action() == EnemyAction::Run);
    // Then, within ten of it, it runs straight away from it.
    const auto apart = [&] {
        return glm::distance(enemies.positionOf(*bomber), enemies.positionOf(*chaser));
    };
    REQUIRE(apart() < 10.0f);
    const f32 before = apart();
    for (s32 i = 0; i < 4 && enemies.alive(*bomber); ++i) {
        enemies.update(kTicks, kStep, near);
    }
    CHECK(apart() > before);
}

TEST_CASE("a placed archer does not abandon its perch when a nearby bomber lights its fuse",
          "[enemies][bomber-placement][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 13, {}, 2);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.placed = true;
    spawn.tier = kSuicideStrength;
    spawn.position = {-30, 0, 0};
    const auto bomber = enemies.spawn(spawn, {});
    REQUIRE(bomber);
    spawn.tier = kArcherStrength;
    spawn.algorithm = kThrowWay;
    spawn.position = {-24, 0, 0};
    const auto archer = enemies.spawn(spawn, {});
    REQUIRE(archer);
    const std::array players{playerAt({-30, 0, 12})};
    bool lit = false;
    for (s32 frame = 0; frame < 100; ++frame) {
        enemies.update(kTicks, kStep, players);
        lit |= enemies.alive(*bomber) && enemies.animatorOf(*bomber)->action() == EnemyAction::Run;
        CHECK(enemies.positionOf(*archer) == spawn.position);
    }
    REQUIRE(lit);
}

TEST_CASE("a way of nought is filled in by kind and strength as the original does",
          "[game][enemies]") {
    constexpr s32 kDemonKind = 2;
    // Unset: the medium kinds chase, the strength-three casters cast, the variants throw,
    // lob and run at the party.
    CHECK(resolvedWayOf(kGruntKind, 1, 0, false) == 7);
    CHECK(resolvedWayOf(kGruntKind, 3, 0, false) == 7);
    CHECK(resolvedWayOf(kDemonKind, 3, 0, false) == 30);
    CHECK(resolvedWayOf(kDemonKind, 2, 0, false) == 7);
    CHECK(resolvedWayOf(kGruntKind, kArcherStrength, 0, false) == kThrowWay);
    CHECK(resolvedWayOf(kGruntKind, kBomberStrength, 0, false) == kBombWay);
    CHECK(resolvedWayOf(kGruntKind, kSuicideStrength, 0, false) == kSuicideWay);
    // A way given stands; one out of range is the kind's own, not the variant's.
    CHECK(resolvedWayOf(kGruntKind, kArcherStrength, kSkirmishWay, false) == kSkirmishWay);
    CHECK(resolvedWayOf(kGruntKind, kArcherStrength, -1, false) == 7);
    CHECK(resolvedWayOf(kDeathKind, 1, -1, false) == 3);
    // Garm's brood lunges and IT lurks, whatever they are placed with.
    for (const s32 way : {-1, 0, kChaseWay, kSeekWay}) {
        CAPTURE(way);
        CHECK(resolvedWayOf(kGarmBroodKind, 3, way, false) == kLungeWay);
        CHECK(resolvedWayOf(kItKind, 1, way, false) == kLurkWay);
    }
    // The small kinds prowl one way or the other, unless told which.
    CHECK(resolvedWayOf(kRatKind, 1, 7, false) == 2);
    CHECK(resolvedWayOf(kRatKind, 1, 0, true) == 4);
    CHECK(resolvedWayOf(kRatKind, 1, 4, false) == 4);
    // Ways one and ten are nought and seven.
    CHECK(resolvedWayOf(kGruntKind, 1, 1, false) == 0);
    CHECK(resolvedWayOf(kGruntKind, 1, 10, false) == 7);
}

TEST_CASE("a placement past the known variants keeps its tier's body", "[game][enemies][assets]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, unpackedRoot(), nullptr, 13, EnemyScales{}, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    EnemySpawn spawn;
    spawn.kind = kGruntKind;
    spawn.tier = kSuicideStrength + 1; // retail's F, which no archive ships
    spawn.placed = true;
    const auto id = enemies.spawn(spawn, {});
    REQUIRE(id.has_value());
    CHECK(enemies.variantOf(*id) == kSuicideStrength + 1);
    REQUIRE(enemies.animatorOf(*id) != nullptr);
    CHECK(enemies.animatorOf(*id)->has(EnemyAction::Walk));
}
TEST_CASE("IT tags the player it touches and is gone unharmed; the swarm seeks who is it",
          "[game][enemies][it]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, itArchive(), nullptr, 3, {}, 1);
    REQUIRE(enemies.loadKind(kItKind));
    const auto it = enemies.spawn({.kind = kItKind, .tier = 1, .placed = true}, {});
    REQUIRE(it);
    // Nothing harms IT (damage_enemy).
    const f32 health = enemies.healthOf(*it);
    EnemyHit hit;
    hit.damage = 1000.0f;
    hit.player = 1;
    enemies.hurt(*it, hit);
    CHECK(enemies.healthOf(*it) == health);
    CHECK(enemies.takeLosses().empty());
    // A second IT far off goes for who is it, seen or not, over the nearer player.
    const auto other =
        enemies.spawn({.kind = kItKind, .tier = 1, .position = {0, 0, -60}, .placed = true}, {});
    REQUIRE(other);
    std::array players{playerAt({0, 0, 6}, 1), playerAt({0, 0, -50}, 3)};
    players[0].it = true;
    enemies.update(kTicks, kStep, players);
    CHECK(enemies.targetOf(*other) == 1);
    players[0].it = false;
    players[1].position = Vec3{0, 0, -500}; // out of the other's sight
    // It walks up to the player it sees and, touching, tags them and goes, striking nobody.
    std::vector<s32> tagged;
    for (s32 frame = 0; frame < 600 && tagged.empty(); ++frame) {
        enemies.update(kTicks, kStep, players);
        const std::vector<s32> now = enemies.takeTagged();
        tagged.insert(tagged.end(), now.begin(), now.end());
    }
    CHECK(tagged == std::vector<s32>{1});
    CHECK(enemies.takeBlows().empty());
    const bool standing = enemies.alive(*it) && !enemies.dying(*it);
    CHECK_FALSE(standing);
    for (s32 frame = 0; frame < 60; ++frame) {
        enemies.update(kTicks, kStep, players);
    }
    CHECK(enemies.takeTagged().empty()); // once only
    CHECK(enemies.takeLosses().empty());
}

TEST_CASE("IT stands with no archive of its own: unseen, and aimed at by nothing",
          "[game][enemies][it]") {
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, test::scratchDirectory("it-bodiless"), nullptr, 2, {}, 1);
    REQUIRE(enemies.loadKind(kItKind));
    CHECK_FALSE(enemies.loadKind(kGruntKind)); // the rest still need theirs
    const auto it = enemies.spawn({.kind = kItKind, .tier = 1, .placed = true}, {});
    REQUIRE(it);
    CHECK(enemies.alive(*it));
    CHECK(enemies.targets().empty());
    CHECK_FALSE(enemies.struckBy(Vec3{0, 3, -5}, Vec3{0, 3, 5}, 1.0f).has_value());
    const std::array players{playerAt({0, 0, 20}, 0)};
    enemies.update(kTicks, kStep, players);
    enemies.draw(device, Mat4{1}, WorldLighting{});
    CHECK(device.draws.empty());
}

} // namespace
