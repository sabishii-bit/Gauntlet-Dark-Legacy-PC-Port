#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
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
#include "game/enemies/Enemies.h"
#include "game/world/HazardSurfaces.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;
constexpr Vec3 kFarEye{0.0f, 1.0e6f, 0.0f}; ///< so high that a shadow is pulled straight up

std::filesystem::path unpackedRoot() {
    return test::unpackedOrSkip("MONSTERS/GRU/animations.json")
        .parent_path()
        .parent_path()
        .parent_path();
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

TEST_CASE("a grunt is bred ahead of its generator, chases the player it sees and strikes on touch",
          "[game][enemies][unpacked]") {
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

TEST_CASE("a grunt struck flinches, thrown down gets up, and killed is worth its experience",
          "[game][enemies][unpacked]") {
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
          "[game][enemies][unpacked]") {
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
          "[game][enemies][unpacked]") {
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
          "[enemies][damage][unpacked]") {
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
          "[game][enemies][enemy-feedback][unpacked]") {
    const auto root = unpackedRoot();
    test::unpackedOrSkip("WEAPONS/animations.json");
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
          "[game][enemies][enemy-feedback][death-retirement][unpacked]") {
    const auto root = unpackedRoot();
    test::unpackedOrSkip("WEAPONS/animations.json");
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
TEST_CASE("invisibility breaks swarm targeting without removing the physical player",
          "[game][items][enemies][unpacked]") {
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
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"GRU1",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"READY","frames":10,"rate":30},
                     {"name":"WALK","frames":10,"rate":30}]}]})");
    return root;
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
          "[game][enemies][unpacked][death-shot]") {
    // kill_enemy's fn_8004F1DC: toward its target, else the first standing player; nobody
    // standing, nothing.
    test::unpackedOrSkip("MONSTERS/GRM/animations.json");
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
          "[game][enemies][shadow][unpacked]") {
    const auto root = unpackedRoot();
    test::unpackedOrSkip("MONSTERS/ZOM/animations.json");
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
          "[game][enemies][unpacked]") {
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
    spawn.algorithm = kWanderWay;
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

TEST_CASE("an enemy on a burning floor is burned every update it stands there",
          "[game][enemies][hazards][unpacked]") {
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
    enemies.update(kTicks, kStep, {});
    const f32 once = enemies.healthOf(*id);
    REQUIRE(once < whole);
    enemies.update(kTicks, kStep, {});
    CHECK(enemies.healthOf(*id) == Approx(once - (whole - once)));
    CHECK(enemies.takeLosses().empty()); // the world's harm is worth nothing to anyone
    enemies.close();
}

TEST_CASE("the swarm runs from a lit suicide bomber near it", "[game][enemies][unpacked]") {
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

TEST_CASE("a placement past the known variants keeps its tier's body",
          "[game][enemies][unpacked]") {
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
