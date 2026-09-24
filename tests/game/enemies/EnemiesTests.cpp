#include <cmath>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Enemies.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;

std::filesystem::path unpackedRoot() {
    return test::unpackedOrSkip("MONSTERS/GRU/animations.json")
        .parent_path()
        .parent_path()
        .parent_path();
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
    REQUIRE(blows[7].knocksBack);
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
    for (s32 i = 0; i < 10; ++i) {
        enemies.update(kTicks, kStep, party);
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
    spawn.algorithm = kStandWay;
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
    const std::array<EnemyView, 1> watching{playerAt(strong.positionOf(0) + Vec3{0, 0, 10})};
    REQUIRE_FALSE(strong.spawn(spawn, watching).has_value());
    REQUIRE(strong.tierOf(0) == 3);
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
    REQUIRE(bred.algorithmOf(*vermin) == 2);
    const std::vector<EnemyView> near{playerAt(Vec3{0.0f, 0.0f, 5.0f})};
    for (s32 i = 0; i < 20; ++i) {
        bred.update(kTicks, kStep, near);
    }
    REQUIRE(bred.algorithmOf(*vermin) == 0);
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
    CHECK(feedback[0].sound() == "S_GRU2HIT1CLOSE");
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
    CHECK(feedback[0].sound() == "S_GRU2DIECLOSE");
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
} // namespace
