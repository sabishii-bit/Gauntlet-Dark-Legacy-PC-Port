#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMind.h"
#include "game/enemies/EnemyMissiles.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;

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

std::vector<CollisionTriangle> floor() {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    return {
        triangle({-60, 0, -60}, {60, 0, -60}, {60, 0, 60}, up),
        triangle({-60, 0, -60}, {60, 0, 60}, {-60, 0, 60}, up),
    };
}

TEST_CASE("a shot flies straight at its mark and a lob falls on it; a player in the way takes "
          "it, the world ends it, and a lob bursts either way",
          "[game][enemies]") {
    EnemyMissiles missiles;
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 20.0f})};
    // The shot: from four up, at the player's middle, at twenty-five a second.
    missiles.launch(EnemyMissileKind::arrow(), Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 3.0f, 20.0f},
                    1.0f, nullptr, 2);
    REQUIRE(missiles.count() == 1);
    // Twenty-five a second along the ground, rising to fall on the mark under its weight.
    REQUIRE(glm::length(Vec2{missiles.missile(0).velocity.x, missiles.missile(0).velocity.z}) ==
            Approx(25.0f));
    REQUIRE(missiles.missile(0).velocity.z > 24.0f);
    s32 flying = 0;
    std::vector<EnemyMissileHit> hits;
    while (hits.empty() && flying < 120) {
        missiles.update(kStep, nullptr, party);
        hits = missiles.takeHits();
        ++flying;
    }
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].player == 0);
    REQUIRE(hits[0].shooter == 2);
    REQUIRE(hits[0].damage == 10.0f);
    REQUIRE(hits[0].burstRadius == 0.0f);
    CHECK_FALSE(hits[0].worldContact);
    CHECK(hits[0].effect().empty());
    CHECK(hits[0].sound().empty());
    REQUIRE(hits[0].direction.z > 0.9f);
    REQUIRE(flying >= 20); // about eight tenths of a second
    REQUIRE(flying <= 30);
    REQUIRE(missiles.count() == 0);
    // The lob: it rises, comes down where it was aimed, and bursts on the floor there.
    WorldCollision collision;
    collision.build(floor());
    const std::vector<EnemyView> nobody;
    missiles.launch(EnemyMissileKind::bomb(), Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 0.0f, 20.0f}, 1.0f,
                    nullptr, 3);
    REQUIRE(missiles.missile(0).velocity.y > 10.0f);
    REQUIRE(missiles.missile(0).kind.spin.y == 1.0f);
    f32 highest = 0.0f;
    hits.clear();
    flying = 0;
    while (hits.empty() && flying < 200) {
        missiles.update(kStep, &collision, nobody);
        if (missiles.count() > 0) {
            highest = std::max(highest, missiles.missile(0).position.y);
        }
        hits = missiles.takeHits();
        ++flying;
    }
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].player == -1);
    REQUIRE(hits[0].burstRadius == 3.0f);
    CHECK(hits[0].worldContact);
    CHECK(hits[0].effect() == "EXPSMALL");
    CHECK(hits[0].sound() == "S_LOBBER_BOMB");
    REQUIRE((hits[0].flags & EnemyMissileKind::kKnockBack) != 0);
    REQUIRE(highest > 5.0f);
    REQUIRE(hits[0].position.z == Approx(20.0f).margin(1.5f));
    REQUIRE(hits[0].position.y < 0.5f);
    // A shot with nothing in its way just goes after its life; a lob bursts where it is.
    missiles.launch(EnemyMissileKind::arrow(), Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 4.0f, 100.0f},
                    1.0f, nullptr, 0);
    for (s32 i = 0; i < 200; ++i) {
        missiles.update(kStep, nullptr, nobody);
    }
    REQUIRE(missiles.count() == 0);
    CHECK(missiles.takeHits().empty());
    missiles.launch(EnemyMissileKind::bomb(), Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 4.0f, 100.0f},
                    1.0f, nullptr, 0);
    s32 lived = 0;
    std::vector<EnemyMissileHit> expired;
    while (expired.empty() && lived < 200) {
        missiles.update(kStep, nullptr, nobody);
        expired = missiles.takeHits();
        ++lived;
    }
    REQUIRE(expired.size() == 1);
    CHECK_FALSE(expired[0].worldContact);
    CHECK(expired[0].burstRadius == 3.0f);
    CHECK(expired[0].effect() == "EXPSMALL");
    CHECK(lived == Approx(EnemyMissiles::kLife * 30.0f).margin(1.0f));
    // The lob's leaving velocity lands it in the flight its pace gives.
    const Vec3 leave =
        EnemyMissiles::lobVelocity(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 40.0f}, 20.0f);
    REQUIRE(leave.z == Approx(20.0f));
    REQUIRE(leave.y == Approx(0.5f * EnemyMissiles::kGravity * 2.0f));
}

TEST_CASE("enemy wall contacts choose their element effect without inventing bolt sounds",
          "[game][enemies][projectile-impact]") {
    WorldCollision collision;
    collision.build({triangle({-20, -10, 10}, {20, -10, 10}, {0, 40, 10}, {0, 0, -1})});
    for (u32 element = 0; element < 5; ++element) {
        EnemyMissiles missiles;
        missiles.launch(EnemyMissileKind::bolt(10, 25, 0.5f, element), {0, 3, 0}, {0, 3, 20}, 1,
                        nullptr, 1);
        std::vector<EnemyMissileHit> hits;
        for (s32 i = 0; i < 30 && hits.empty(); ++i) {
            missiles.update(kStep, &collision, {});
            hits = missiles.takeHits();
        }
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].worldContact);
        CHECK(hits[0].effect() == (element == 0 ? "SPARKS" : element == 1 ? "FIREHIT" : "HITCOL"));
        CHECK(hits[0].sound().empty());
        missiles.update(kStep, &collision, {});
        CHECK(missiles.takeHits().empty());
    }
}

TEST_CASE("fast enemy shots stop at thin walls before reaching sheltered players",
          "[game][enemies][enemy-projectile-contact]") {
    const f32 dt = GENERATE(1.0f / 120, 1.0f / 60, 1.0f / 30, 0.2f);
    WorldCollision collision;
    collision.build({triangle({-20, -10, 1}, {20, -10, 1}, {0, 40, 1}, {0, 0, -1})});
    EnemyMissiles missiles;
    missiles.launch(EnemyMissileKind::bolt(25, 80, 0.2f, 2), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 4);
    const std::array party{playerAt({0, 0, 4})};
    for (s32 frame = 0; frame < 120 && missiles.count() != 0; ++frame) {
        missiles.update(dt, &collision, party);
    }
    const auto hits = missiles.takeHits();
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].player == -1);
    CHECK(hits[0].worldContact);
    CHECK(hits[0].effect() == "HITCOL");
    CHECK(hits[0].position.z < 1);
    CHECK(hits[0].position.z > 0.5f);
    CHECK(hits[0].direction.z == Approx(1));
    CHECK(missiles.count() == 0);
    missiles.update(dt, &collision, party);
    CHECK(missiles.takeHits().empty());
}

TEST_CASE("enemy projectiles use the first body along their travel and catch descending floor hits",
          "[game][enemies][enemy-projectile-contact]") {
    EnemyMissiles missiles;
    SECTION("nearer body protects the farther player despite reversed party order") {
        const std::array party{playerAt({0, 0, 12}, 0), playerAt({0, 0, 4}, 1)};
        missiles.launch(EnemyMissileKind::bolt(10, 80, 0.2f), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 0);
        missiles.update(0.2f, nullptr, party);
        const auto hits = missiles.takeHits();
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].player == 1);
        CHECK(hits[0].position.z == Approx(2.8f).margin(0.001f));
    }
    SECTION("a coarse descending step cannot skip the floor") {
        WorldCollision collision;
        collision.build(floor());
        missiles.launch(EnemyMissileKind::bolt(10, 80, 0.2f), {0, 4, 0}, {0, -4, 0}, 1, nullptr, 0);
        missiles.update(0.2f, &collision, {});
        const auto hits = missiles.takeHits();
        REQUIRE(hits.size() == 1);
        CHECK(hits[0].worldContact);
        CHECK(hits[0].position.y == Approx(0.2f).margin(0.001f));
    }
    SECTION("paused time neither moves a projectile nor applies a contact") {
        const std::array party{playerAt({0, 0, 0})};
        missiles.launch(EnemyMissileKind::arrow(), {0, 3, 0}, {0, 3, 10}, 1, nullptr, 0);
        missiles.update(0, nullptr, party);
        missiles.update(-1, nullptr, party);
        REQUIRE(missiles.count() == 1);
        CHECK(missiles.takeHits().empty());
        CHECK(missiles.missile(0).position == Vec3{0, 3, 0});
    }
}

TEST_CASE("every kind throws what the original's table gives it, from the slot its way uses",
          "[game][enemies]") {
    // The medium kinds share the arrow and the bomb; the small have nothing.
    REQUIRE(enemyMissileOf(13, EnemyMissileKind::kArrow)->damage == 10.0f);
    REQUIRE(enemyMissileOf(13, EnemyMissileKind::kBomb)->burstRadius == 3.0f);
    REQUIRE(enemyMissileOf(4, EnemyMissileKind::kArrow)->speed == 25.0f);
    REQUIRE_FALSE(enemyMissileOf(3, EnemyMissileKind::kArrow).has_value());
    REQUIRE_FALSE(enemyMissileOf(13, 2).has_value());
    // The ghost's and demon's bolt, the sorcerer's and warlock's, the garm's, the worm's three.
    REQUIRE(enemyMissileOf(20, 2)->damage == 15.0f);
    REQUIRE(enemyMissileOf(20, 2)->flags == 0x1);
    REQUIRE(enemyMissileOf(20, 2)->burstRadius == 0.0f);
    REQUIRE(enemyMissileOf(7, 2)->damage == 20.0f);
    REQUIRE(enemyMissileOf(27, 2)->speed == 80.0f);
    REQUIRE(enemyMissileOf(17, 0)->damage == 5.0f);
    REQUIRE(enemyMissileOf(17, 2)->damage == 15.0f);
    REQUIRE_FALSE(enemyMissileOf(17, 3).has_value());
    // The way says the slot.
    REQUIRE(missileSlotOfWay(kSkirmishWay) == EnemyMissileKind::kArrow);
    REQUIRE(missileSlotOfWay(kThrowWay) == EnemyMissileKind::kArrow);
    REQUIRE(missileSlotOfWay(kBombWay) == EnemyMissileKind::kBomb);
    REQUIRE(missileSlotOfWay(26) == EnemyMissileKind::kBomb);
    REQUIRE(missileSlotOfWay(28) == 2);
    REQUIRE(missileSlotOfWay(kChaseWay) == 2);
}

TEST_CASE("the thrower shoots on its wait, the skirmisher keeps its distance, and the suicide "
          "lights its fuse and runs",
          "[game][enemies][mind]") {
    MindSense sense;
    sense.ticks = 2;
    sense.target = 0;
    sense.targetPosition = Vec3{0.0f, 0.0f, 20.0f};
    sense.targetDistance = 20.0f;
    sense.sight = 30.0f;
    sense.idleTicks = 100;
    sense.recognized = true;
    // The thrower stands, faces, and throws; then waits its idle out.
    const EnemyMind& thrower = enemyMindOf(kThrowWay);
    REQUIRE(enemyMindOf(kBombWay).name() == "throw");
    MindMemory memory;
    MindIntent intent = thrower.think(memory, sense);
    REQUIRE(intent.pace == 0.0f);
    REQUIRE(intent.throwing);
    REQUIRE(intent.heading == Approx(0.0f));
    sense.threw = true;
    intent = thrower.think(memory, sense);
    sense.threw = false;
    REQUIRE(memory.fuse == 100);
    s32 waited = 0;
    while (!thrower.think(memory, sense).throwing && waited < 100) {
        ++waited;
    }
    REQUIRE(waited == 49); // the tick of the throw counted too
    // Too high above it, or out of sight: no throw.
    sense.targetVertical = 12.0f;
    REQUIRE_FALSE(thrower.think(memory, sense).throwing);
    sense.targetVertical = 0.0f;
    sense.targetDistance = 31.0f;
    REQUIRE_FALSE(thrower.think(memory, sense).throwing);
    // The skirmisher throws from range, backs off within eighteen (six tenths of thirty)
    // facing its player, until beyond twenty-four.
    const EnemyMind& skirmisher = enemyMindOf(kSkirmishWay);
    MindMemory archer;
    sense.targetDistance = 25.0f;
    intent = skirmisher.think(archer, sense);
    REQUIRE(intent.throwing);
    REQUIRE_FALSE(archer.keepingOff);
    sense.targetDistance = 17.0f;
    intent = skirmisher.think(archer, sense);
    REQUIRE(archer.keepingOff);
    REQUIRE_FALSE(intent.throwing);
    REQUIRE(intent.pace == Approx(0.8f));
    REQUIRE(std::abs(intent.heading) == Approx(3.1415927f));
    REQUIRE_FALSE(intent.turn);
    REQUIRE(intent.action == EnemyAction::RunAttack);
    sense.targetDistance = 22.0f;
    REQUIRE(skirmisher.think(archer, sense).pace == Approx(0.8f)); // still backing off
    sense.targetDistance = 25.0f;
    intent = skirmisher.think(archer, sense);
    REQUIRE_FALSE(archer.keepingOff);
    REQUIRE(intent.throwing);
    // The suicide waits until someone is in sight, a second of fuse, then runs and blows up.
    const EnemyMind& suicide = enemyMindOf(kSuicideWay);
    MindMemory bomber;
    MindSense alone;
    alone.ticks = 2;
    intent = suicide.think(bomber, alone);
    REQUIRE(bomber.mode == 0);
    REQUIRE(intent.pace == 0.0f);
    sense.targetDistance = 20.0f;
    intent = suicide.think(bomber, sense);
    REQUIRE(bomber.mode == 1);
    REQUIRE(bomber.fuse == 60);
    for (s32 i = 0; i < 29; ++i) {
        intent = suicide.think(bomber, sense);
    }
    REQUIRE(bomber.mode == 1);
    REQUIRE(intent.action == EnemyAction::Ready);
    intent = suicide.think(bomber, sense);
    REQUIRE(intent.action == EnemyAction::ReadyToWalk); // the fuse lit
    sense.action = EnemyAction::ReadyToWalk;
    intent = suicide.think(bomber, sense);
    REQUIRE(bomber.mode == 2);
    CHECK(intent.yell); // it cries out as the run starts, once
    intent = suicide.think(bomber, sense);
    CHECK_FALSE(intent.yell);
    REQUIRE(intent.pace == Approx(1.5f));
    REQUIRE(intent.action == EnemyAction::Run);
    REQUIRE_FALSE(intent.explode);
    sense.contact = 0;
    REQUIRE(suicide.think(bomber, sense).explode);
    sense.contact = -1;
    for (s32 i = 0; i < 118; ++i) {
        intent = suicide.think(bomber, sense);
    }
    REQUIRE(intent.explode); // four seconds of running
}

TEST_CASE("a zombie archer shoots the player it sees, a bomber lobs, and a suicide blows up "
          "against them",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root = test::unpackedOrSkip("MONSTERS/ZOM/animations.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(floor());
    Enemies enemies;
    EnemyScales scales;
    scales.damage = 0.5f;
    enemies.open(device, root, &collision, 13, scales, 5);
    REQUIRE(enemies.loadKind(13));
    EnemyMissiles missiles;
    // The archer: placed at strength four with the skirmisher's way, as the levels place
    // them, the archer's body and a second-tier's health.
    EnemySpawn spawn;
    spawn.kind = 13;
    spawn.tier = kArcherStrength;
    spawn.algorithm = kSkirmishWay;
    spawn.placed = true;
    spawn.position = Vec3{0.0f, 0.0f, 0.0f};
    spawn.idleTicks = 90;
    const auto archer = enemies.spawn(spawn, {});
    REQUIRE(archer.has_value());
    REQUIRE(enemies.variantOf(*archer) == kArcherStrength);
    REQUIRE(enemies.tierOf(*archer) == 2);
    REQUIRE(enemies.algorithmOf(*archer) == kSkirmishWay);
    REQUIRE(enemies.healthOf(*archer) == Approx(30.0f * 0.333f * 2.0f));
    REQUIRE(enemies.animatorOf(*archer)->has(EnemyAction::Throw));
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 25.0f})};
    s32 until = 0;
    while (missiles.count() == 0 && until < 300) {
        enemies.update(kTicks, kStep, party, {}, &missiles, 1.0f);
        ++until;
    }
    REQUIRE(missiles.count() == 1);
    REQUIRE(missiles.missile(0).shooter == *archer);
    REQUIRE(missiles.missile(0).kind.burstRadius == 0.0f);
    REQUIRE(missiles.missile(0).velocity.z > 20.0f);
    // The arrow twangs as it goes, from the archer's middle.
    const auto shots = enemies.takeCues();
    REQUIRE(shots.size() == 1);
    CHECK(shots[0].kind == EnemyCue::Kind::Arrow);
    CHECK(shots[0].enemyKind == 13);
    CHECK(shots[0].position.y > 0.0f);
    REQUIRE(enemies.positionOf(*archer) == Vec3{0.0f, 0.0f, 0.0f}); // it stood to shoot
    std::vector<EnemyMissileHit> hits;
    for (s32 i = 0; i < 120 && hits.empty(); ++i) {
        missiles.update(kStep, &collision, party);
        hits = missiles.takeHits();
    }
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].player == 0);
    // A player close by is backed away from, the archer still facing them.
    const std::vector<EnemyView> close{playerAt(Vec3{0.0f, 0.0f, 10.0f})};
    for (s32 i = 0; i < 60; ++i) {
        enemies.update(kTicks, kStep, close, {}, &missiles, 1.0f);
    }
    REQUIRE(enemies.positionOf(*archer).z < -1.0f);
    REQUIRE(std::abs(enemies.yawOf(*archer)) < 0.5f);
    // The bomber lobs; given no way, its strength gives it the lobber's.
    spawn.tier = kBomberStrength;
    spawn.algorithm = 0;
    SECTION("stationary bomber") {}
    SECTION("retreating bomber uses the archer movement with bomb ammunition") {
        spawn.algorithm = kSkirmishBombWay;
    }
    spawn.position = Vec3{30.0f, 0.0f, 0.0f};
    const auto bomber = enemies.spawn(spawn, {});
    REQUIRE(bomber.has_value());
    REQUIRE(enemies.algorithmOf(*bomber) ==
            (spawn.algorithm == kSkirmishBombWay ? kSkirmishBombWay : kBombWay));
    missiles.clear();
    const std::vector<EnemyView> afar{playerAt(Vec3{30.0f, 0.0f, 25.0f})};
    for (s32 i = 0; i < 300 && missiles.count() == 0; ++i) {
        enemies.update(kTicks, kStep, afar, {}, &missiles, 1.0f);
    }
    bool lobbed = false;
    for (usize m = 0; m < missiles.count(); ++m) {
        lobbed = lobbed || (missiles.missile(m).shooter == *bomber &&
                            missiles.missile(m).kind.burstRadius > 0.0f);
    }
    REQUIRE(lobbed);
    // The suicide: a first-tier body, a fuse, a run, and a blast of fifty at the level's
    // half, dead of it.
    spawn.tier = kSuicideStrength;
    spawn.algorithm = 0;
    spawn.position = Vec3{-30.0f, 0.0f, 0.0f};
    const auto suicide = enemies.spawn(spawn, {});
    REQUIRE(suicide.has_value());
    REQUIRE(enemies.tierOf(*suicide) == 1);
    REQUIRE(enemies.algorithmOf(*suicide) == kSuicideWay);
    const std::vector<EnemyView> near{playerAt(Vec3{-30.0f, 0.0f, 12.0f})};
    std::vector<EnemyBurst> bursts;
    s32 yells = 0;
    enemies.takeFeedback();
    for (s32 i = 0; i < 600 && bursts.empty(); ++i) {
        enemies.update(kTicks, kStep, near, {}, &missiles, 1.0f);
        bursts = enemies.takeBursts();
        for (const EnemyCue& cue : enemies.takeCues()) {
            yells += cue.kind == EnemyCue::Kind::Yell ? 1 : 0;
        }
    }
    REQUIRE(bursts.size() == 1);
    REQUIRE(bursts[0].enemy == *suicide);
    REQUIRE(bursts[0].damage == 25.0f);
    REQUIRE(bursts[0].position.z > 5.0f); // it ran most of the way
    REQUIRE_FALSE(enemies.alive(*suicide));
    CHECK(yells == 1);
    // Its own fuse lets the yell run on, and it dies without a cry or a burst of blood.
    CHECK_FALSE(bursts[0].silencesYell);
    CHECK(enemies.takeFeedback().empty());
    // Shot down before it gets there, it goes up all the same, once (enemy_dies).
    spawn.position = Vec3{30.0f, 0.0f, 0.0f};
    const auto shot = enemies.spawn(spawn, {});
    REQUIRE(shot.has_value());
    EnemyHit slay;
    slay.damage = 100.0f;
    slay.player = 0;
    enemies.hurt(*shot, slay);
    bursts = enemies.takeBursts();
    REQUIRE(bursts.size() == 1);
    REQUIRE(bursts[0].enemy == *shot);
    REQUIRE(bursts[0].damage == 25.0f);
    CHECK(bursts[0].silencesYell);
    CHECK(enemies.takeFeedback().size() == 1);
    enemies.hurt(*shot, slay);
    for (s32 i = 0; i < 30; ++i) {
        enemies.update(kTicks, kStep, near, {}, &missiles, 1.0f);
    }
    REQUIRE(enemies.takeBursts().empty());
}

TEST_CASE("a strength-three demon casts its own fireball from afar and fights hand to hand",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root = test::unpackedOrSkip("MONSTERS/DEM/animations.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    constexpr s32 kDemonKind = 2;
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(floor());
    Enemies enemies;
    enemies.open(device, root, &collision, 13, {}, 5);
    REQUIRE(enemies.loadKind(kDemonKind));
    EnemySpawn spawn;
    spawn.kind = kDemonKind;
    spawn.tier = 3;
    spawn.algorithm = 0; // unset: a third-strength caster casts
    spawn.placed = true;
    const auto demon = enemies.spawn(spawn, {});
    REQUIRE(demon.has_value());
    REQUIRE(enemies.algorithmOf(*demon) == kCastWay);
    EnemyMissiles missiles;
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 20.0f})};
    for (s32 i = 0; i < 300 && missiles.count() == 0; ++i) {
        enemies.update(kTicks, kStep, party, {}, &missiles, 1.0f);
    }
    REQUIRE(missiles.count() == 1);
    const EnemyMissile& ball = missiles.missile(0);
    CHECK(ball.shooter == *demon);
    CHECK(ball.kind.damage == 15.0f); // the demon's bolt, the third slot
    REQUIRE(ball.model != nullptr);   // DEM_FBALL
    CHECK(ball.velocity.z > 0.0f);
    CHECK(enemies.takeBlows().empty());
}

TEST_CASE("a range-keeping sorcerer backs off, then casts its own bolt, and only ahead of it",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root = test::unpackedOrSkip("MONSTERS/SOR/animations.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    constexpr s32 kSorcererKind = 7;
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build(floor());
    Enemies enemies;
    enemies.open(device, root, &collision, 13, {}, 5);
    REQUIRE(enemies.loadKind(kSorcererKind));
    EnemySpawn spawn;
    spawn.kind = kSorcererKind;
    spawn.tier = 3;
    spawn.algorithm = kRangeCastWay;
    spawn.placed = true;
    const auto sorcerer = enemies.spawn(spawn, {});
    REQUIRE(sorcerer.has_value());
    // Too close: it backs away from the player before it casts.
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 7.0f})};
    EnemyMissiles missiles;
    for (s32 i = 0; i < 300 && missiles.count() == 0; ++i) {
        enemies.update(kTicks, kStep, party, {}, &missiles, 1.0f);
    }
    REQUIRE(missiles.count() == 1);
    CHECK(enemies.positionOf(*sorcerer).z < -1.0f);
    const EnemyMissile& bolt = missiles.missile(0);
    CHECK(bolt.shooter == *sorcerer);
    CHECK(bolt.kind.damage == 20.0f); // the sorcerer's own, the third slot
    REQUIRE(bolt.model != nullptr);   // SOR_FBALL
    CHECK(enemies.takeBlows().empty());
}

TEST_CASE("invisibility does not make an enemy arrow pass through its victim",
          "[game][items][enemies]") {
    EnemyMissiles missiles;
    auto victim = playerAt({0, 0, 10});
    victim.invisible = true;
    std::vector<EnemyView> party{victim};
    missiles.launch(EnemyMissileKind::arrow(), {0, 3, 0}, {0, 3, 10}, 1, nullptr, 2);
    for (s32 tick = 0; tick < 30; ++tick) {
        missiles.update(kStep, nullptr, party);
    }
    const auto hits = missiles.takeHits();
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].player == 0);
}

TEST_CASE("the swarm's shots and lobs are aimed as the original aims them, never leading",
          "[game][enemies][enemy-aim]") {
    // An arrow at twenty: a fixed pace along the ground, rising so as to fall on a point
    // three and a half under the aim under its weight of thirty.
    const Vec3 arrow =
        EnemyMissiles::heading(EnemyMissileKind::arrow(), {0, 0, 0}, {0, 0, 20}, 25.0f, 0.0f);
    CHECK(arrow.x == Approx(0.0f));
    CHECK(arrow.z == Approx(1.0f));
    CHECK(arrow.y == Approx(0.5f * 30.0f * 20.0f / 625.0f - 3.5f / 20.0f));
    // A lob aims five and a half under, and an error lifts or drops it.
    const Vec3 lob =
        EnemyMissiles::heading(EnemyMissileKind::bomb(), {0, 0, 0}, {0, 0, 20}, 20.0f, 2.0f);
    CHECK(lob.y == Approx(0.5f * 35.0f * 20.0f / 400.0f + (2.0f - 5.5f) / 20.0f));
    // A bolt goes straight, but never downward.
    const EnemyMissileKind bolt = *enemyMissileOf(7, EnemyMissileKind::kBolt);
    const Vec3 up = EnemyMissiles::heading(bolt, {0, 0, 0}, {0, 3, 4}, 20.0f, 0.0f);
    CHECK(glm::length(up) == Approx(1.0f));
    CHECK(up.y == Approx(0.6f));
    CHECK(EnemyMissiles::heading(bolt, {0, 4, 0}, {0, 0, 3}, 20.0f, 0.0f).y == 0.0f);

    // It leaves three ahead of the kind's launch point, faced within an eighth of a turn.
    EnemyMissiles missiles;
    EnemyMissileLaunch launch;
    launch.body = Vec3{0.0f, 3.0f, 0.0f};
    launch.target = Vec3{0.0f, 3.0f, 20.0f};
    launch.point = enemyLaunchPointOf(4, EnemyMissileKind::kArrow);
    REQUIRE(missiles.launch(EnemyMissileKind::arrow(), launch));
    const EnemyMissile& shot = missiles.missile(0);
    const Vec3 way =
        EnemyMissiles::heading(EnemyMissileKind::arrow(), launch.body, launch.target, 25.0f, 0.0f);
    CHECK(shot.position.x == Approx(0.0f));
    CHECK(shot.position.y == Approx(3.0f + 1.5f + 3.0f * way.y));
    CHECK(shot.position.z == Approx(3.0f));
    CHECK(shot.velocity.z == Approx(25.0f));
    launch.facing = 1.0f; // more than an eighth of a turn off
    CHECK_FALSE(missiles.launch(EnemyMissileKind::arrow(), launch));
    launch.facing = 0.7f; // within it
    CHECK(missiles.launch(EnemyMissileKind::arrow(), launch));
    // A wall between the body and where it would leave keeps it in hand.
    WorldCollision wall;
    wall.build({triangle({-20, -10, 1.5f}, {20, -10, 1.5f}, {0, 40, 1.5f}, {0, 0, -1})});
    launch.facing = 0.0f;
    CHECK_FALSE(missiles.launch(EnemyMissileKind::arrow(), launch, &wall));
    CHECK(missiles.count() == 2);

    // The level's aim strays a lob up or down by up to two and a half times itself.
    launch.aimError = 1.0f;
    launch.point = enemyLaunchPointOf(4, EnemyMissileKind::kBomb);
    f32 lowest = 100.0f;
    f32 highest = -100.0f;
    for (s32 i = 0; i < 64; ++i) {
        EnemyMissiles lobs(static_cast<u32>(i));
        REQUIRE(lobs.launch(EnemyMissileKind::bomb(), launch));
        lowest = std::min(lowest, lobs.missile(0).velocity.y);
        highest = std::max(highest, lobs.missile(0).velocity.y);
    }
    const f32 plain =
        EnemyMissiles::heading(EnemyMissileKind::bomb(), launch.body, launch.target, 20.0f, 0.0f)
            .y *
        20.0f;
    CHECK(lowest < plain);
    CHECK(highest > plain);
    CHECK(highest - lowest <= EnemyMissiles::kAimSpread + 0.01f);
}

TEST_CASE("the kinds' launch points and the flags their hits carry follow the original's table",
          "[game][enemies][enemy-aim]") {
    CHECK(enemyLaunchPointOf(1, EnemyMissileKind::kArrow).height == 2.5f);
    CHECK(enemyLaunchPointOf(1, EnemyMissileKind::kBomb).height == 2.5f);
    CHECK(enemyLaunchPointOf(2, EnemyMissileKind::kBolt).height == 0.0f);
    CHECK(enemyLaunchPointOf(4, EnemyMissileKind::kArrow).height == 1.5f);
    CHECK(enemyLaunchPointOf(13, EnemyMissileKind::kArrow).height == 1.0f);
    CHECK(enemyLaunchPointOf(7, EnemyMissileKind::kBolt).height == 1.5f);
    CHECK(enemyLaunchPointOf(24, EnemyMissileKind::kBolt).height == 1.5f);
    CHECK(enemyLaunchPointOf(14, EnemyMissileKind::kBolt).height == 1.0f);
    CHECK(enemyLaunchPointOf(14, EnemyMissileKind::kArrow).height == 1.5f);
    CHECK(enemyLaunchPointOf(17, EnemyMissileKind::kBolt).height == 2.0f);
    CHECK(enemyLaunchPointOf(17, EnemyMissileKind::kBolt).shift == 2.0f);
    CHECK(enemyLaunchPointOf(23, EnemyMissileKind::kArrow).height == 0.0f);
    CHECK(enemyLaunchPointOf(23, EnemyMissileKind::kBomb).shift == -2.5f);
    CHECK(enemyLaunchPointOf(27, EnemyMissileKind::kBolt).height == 0.0f);
    // An arrow's hit is heard as an arrow, a bolt's as a bolt; a lob knocks back.
    CHECK(EnemyMissileKind::arrow().hitFlags() == EnemyMissileKind::kArrowHit);
    CHECK(enemyMissileOf(20, 2)->hitFlags() == (0x1 | EnemyMissileKind::kBoltHit));
    CHECK(EnemyMissileKind::bomb().hitFlags() == EnemyMissileKind::kKnockBack);
    // Weights: the arrow thirty, the bomb thirty-five, the bolts one, the garm's none.
    CHECK(EnemyMissileKind::arrow().weight == 30.0f);
    CHECK(EnemyMissileKind::bomb().weight == 35.0f);
    CHECK(enemyMissileOf(7, 2)->weight == 1.0f);
    CHECK(enemyMissileOf(27, 2)->weight == 0.0f);
    CHECK(enemyMissileOf(17, 1)->slot == EnemyMissileKind::kBomb);
}

TEST_CASE("a lob's burst grows as its harm fades, reaching each player once and the swarm",
          "[game][enemies][enemy-burst]") {
    EnemyMissiles missiles;
    // Struck directly, player 0 is spared the burst; player 1 stands two and a half out,
    // player 2 far away.
    const std::array party{playerAt({0, -3, 0}, 0), playerAt({2.5f, -3, 0}, 1),
                           playerAt({20, -3, 0}, 2)};
    missiles.burst({0, 0, 0}, 3.0f, 10.0f, EnemyMissileKind::kKnockBack, 1.0f, 0);
    CHECK(missiles.burstCount() == 1);
    std::vector<PointLight> lights;
    missiles.lights(lights);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].radius == EnemyMissiles::kBombLightRadius);
    CHECK(lights[0].color.x > 0.0f);
    CHECK(lights[0].color.y == 0.0f);
    // One of the swarm a unit off is reached at once; one far off never.
    const std::array swarm{MissileTarget{7, {1, 0, 0}, 0.5f, 4.0f},
                           MissileTarget{8, {30, 0, 0}, 0.5f, 4.0f}};
    std::vector<EnemyMissileHit> hits;
    for (s32 frame = 0; frame < 40; ++frame) {
        missiles.update(kStep, nullptr, party, swarm);
        const auto taken = missiles.takeHits();
        hits.insert(hits.end(), taken.begin(), taken.end());
    }
    CHECK(missiles.burstCount() == 0);
    s32 struck = 0;
    s32 reached = 0;
    for (const EnemyMissileHit& hit : hits) {
        REQUIRE(hit.fromBurst);
        CHECK(hit.burstRadius == 0.0f);
        if (hit.player < 0) {
            ++struck;
            CHECK(hit.target == 7);
            CHECK(hit.damage == Approx(10.0f * 1.5f * (1.0f - 0.33f)));
            CHECK(hit.direction.x == Approx(1.0f));
            continue;
        }
        ++reached;
        CHECK(hit.player == 1);
        // Reached as the burst grew past it, with less than its first harm, pushed a quarter.
        CHECK(hit.damage < 10.0f);
        CHECK(hit.damage > 0.0f);
        CHECK(hit.direction.x == Approx(EnemyMissiles::kBurstPush));
        CHECK(hit.direction.y == 0.0f);
    }
    CHECK(struck == 1);
    CHECK(reached == 1);
    // A weak burst only hurts: under five its knock goes.
    missiles.burst({0, 0, 0}, 3.0f, 3.0f, EnemyMissileKind::kKnockBack, 1.0f, -1);
    missiles.update(kStep, nullptr, std::array{playerAt({0.5f, -3, 0}, 3)});
    for (const EnemyMissileHit& hit : missiles.takeHits()) {
        CHECK((hit.flags & EnemyMissileKind::kKnockBack) == 0);
    }
}

TEST_CASE("only a lob in flight lights its way", "[game][enemies][enemy-burst]") {
    EnemyMissiles missiles;
    missiles.launch(EnemyMissileKind::arrow(), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 0);
    std::vector<PointLight> lights;
    missiles.lights(lights);
    CHECK(lights.empty());
    missiles.launch(EnemyMissileKind::bomb(), {0, 3, 0}, {0, 0, 20}, 1, nullptr, 0);
    missiles.lights(lights);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].position == missiles.missile(1).position);
    missiles.clear();
    lights.clear();
    missiles.lights(lights);
    CHECK(lights.empty());
}

TEST_CASE("a gas blast hangs over its stages, hurting players every half second and the swarm "
          "once a stage",
          "[game][enemies][enemy-burst]") {
    EnemyMissiles missiles;
    EnemyBlast gas;
    gas.position = Vec3{0, 0, 0};
    gas.radius = 7.5f;
    gas.damage = 25.0f;
    gas.flags = EnemyBlast::kGas;
    gas.stages = {2.0f / 3.0f, 2.0f};
    missiles.blast(gas);
    std::vector<PointLight> lights;
    missiles.lights(lights);
    CHECK(lights.empty()); // unlit, unlike a lob's
    const std::array party{playerAt({1, -3, 0}, 0)};
    const std::array swarm{MissileTarget{3, {1, 0, 1}, 1.0f, 4.0f}};
    s32 playerHits = 0;
    s32 swarmHits = 0;
    f32 elapsed = 0.0f;
    std::vector<f32> swarmTimes;
    while (missiles.burstCount() > 0 && elapsed < 5.0f) {
        missiles.update(kStep, nullptr, party, swarm);
        elapsed += kStep;
        for (const EnemyMissileHit& hit : missiles.takeHits()) {
            CHECK((hit.flags & EnemyBlast::kGas) != 0);
            if (hit.player == 0) {
                ++playerHits;
            } else if (hit.target == 3) {
                ++swarmHits;
                swarmTimes.push_back(elapsed);
            }
        }
    }
    CHECK(elapsed == Approx(2.0f / 3.0f + 2.0f).margin(2 * kStep));
    // Harmful for two thirds of each stage: a hit every half second while it is.
    CHECK(playerHits >= 3);
    CHECK(playerHits <= 5);
    // The swarm once in each stage, a second apart at least.
    REQUIRE(swarmHits == 2);
    CHECK(swarmTimes[1] - swarmTimes[0] >= EnemyMissiles::kSwarmGap - kStep);
}

TEST_CASE("a blast does not reach a player sheltered by a wall past ten units",
          "[game][enemies][enemy-burst]") {
    WorldCollision wall;
    wall.build({triangle({-20, -10, 5}, {20, -10, 5}, {0, 40, 5}, {0, 0, -1})});
    for (const f32 z : {11.0f, 4.0f}) {
        EnemyMissiles missiles;
        EnemyBlast blast;
        blast.radius = 20.0f;
        blast.damage = 50.0f;
        blast.stages = {1.0f};
        missiles.blast(blast);
        const std::array party{playerAt({0, -3, z}, 0)};
        s32 hits = 0;
        for (s32 frame = 0; frame < 40; ++frame) {
            missiles.update(kStep, &wall, party);
            hits += static_cast<s32>(missiles.takeHits().size());
        }
        CHECK(hits == (z > 10.0f ? 0 : 1));
    }
}

TEST_CASE("the level's items stop the swarm's missiles, and one in the way keeps a throw in hand",
          "[game][enemies][enemy-missile-items]") {
    Obstacle chest;
    chest.centre = Vec3{0, 0, 10};
    chest.halfAcross = 1.0f;
    chest.halfAlong = 1.0f;
    chest.height = 6.0f;
    const std::array items{MissileStop::of(chest)};
    const std::array party{playerAt({0, 0, 20})};
    EnemyMissiles missiles;
    missiles.launch(EnemyMissileKind::bolt(10, 25, 0.3f), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 0);
    std::vector<EnemyMissileHit> hits;
    for (s32 frame = 0; frame < 60 && hits.empty(); ++frame) {
        missiles.update(kStep, nullptr, party, {}, items);
        hits = missiles.takeHits();
    }
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].player == -1);
    CHECK(hits[0].worldContact);
    CHECK(hits[0].effect() == "SPARKS");
    CHECK(hits[0].position.z < 9.0f);
    // An open gate, a broken barrel: not solid, not in the way.
    Obstacle open = chest;
    open.solid = false;
    missiles.launch(EnemyMissileKind::bolt(10, 25, 0.3f), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 0);
    hits.clear();
    for (s32 frame = 0; frame < 60 && hits.empty(); ++frame) {
        missiles.update(kStep, nullptr, party, {}, std::array{MissileStop::of(open)});
        hits = missiles.takeHits();
    }
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].player == 0);
    // An item where the throw would leave from: nothing goes.
    EnemyMissileLaunch launch;
    launch.body = Vec3{0, 3, 8};
    launch.target = Vec3{0, 3, 20};
    CHECK_FALSE(missiles.launch(EnemyMissileKind::arrow(), launch, nullptr, std::array{chest}));
    CHECK(missiles.launch(EnemyMissileKind::arrow(), launch, nullptr, {}));
}

TEST_CASE("reflecting armour sends a missile back, harmless to its wearer, onto the swarm",
          "[game][enemies][enemy-reflect]") {
    EnemyMissiles missiles;
    auto shielded = playerAt({0, 0, 10});
    shielded.reflects = true;
    const std::array party{shielded};
    // The thrower stands behind where the shot came from.
    const std::array swarm{MissileTarget{4, {0, 0, -5}, 1.0f, 6.0f}};
    missiles.launch(EnemyMissileKind::bolt(20, 25, 0.3f), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 2);
    std::vector<EnemyMissileHit> hits;
    bool turned = false;
    for (s32 frame = 0; frame < 120 && missiles.count() > 0; ++frame) {
        missiles.update(kStep, nullptr, party, swarm);
        if (missiles.count() > 0 && missiles.missile(0).reflected) {
            turned = true;
            CHECK(missiles.missile(0).velocity.z < 0.0f);
            CHECK(missiles.missile(0).kind.damage == EnemyMissiles::kReflectedMost);
            CHECK(missiles.missile(0).secondsLeft < EnemyMissiles::kLife - 1.0f);
        }
        const auto taken = missiles.takeHits();
        hits.insert(hits.end(), taken.begin(), taken.end());
    }
    REQUIRE(turned);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].ricochet);
    CHECK(hits[0].player == -1);
    CHECK(hits[1].target == 4); // back onto the swarm
    CHECK(hits[1].player == -1);
    CHECK(hits[1].damage == EnemyMissiles::kReflectedMost);
    // The ricochet is heard at most once a second.
    missiles.launch(EnemyMissileKind::bolt(20, 25, 0.3f), {0, 3, 0}, {0, 3, 20}, 1, nullptr, 2);
    s32 rings = 0;
    for (s32 frame = 0; frame < 20; ++frame) {
        missiles.update(kStep, nullptr, party);
        for (const EnemyMissileHit& hit : missiles.takeHits()) {
            rings += hit.ricochet ? 1 : 0;
        }
    }
    CHECK(rings == 0);
}

TEST_CASE("the garm's bolt pierces the players it hurts and the items in its way, growing as it "
          "leaves",
          "[game][enemies][enemy-pierce]") {
    const EnemyMissileKind garm = *enemyMissileOf(27, EnemyMissileKind::kBolt);
    REQUIRE(garm.pierces());
    CHECK((garm.hitFlags() & EnemyMissileKind::kPierces) != 0);
    Obstacle chest;
    chest.centre = Vec3{0, 0, 10};
    chest.halfAcross = 1.0f;
    chest.halfAlong = 1.0f;
    chest.height = 6.0f;
    const std::array party{playerAt({0, 0, 20}, 0), playerAt({0, 0, 40}, 1)};
    EnemyMissiles missiles;
    missiles.launch(garm, {0, 3, 0}, {0, 3, 60}, 1, nullptr, 5);
    std::vector<EnemyMissileHit> hits;
    for (s32 frame = 0; frame < 30 && missiles.count() > 0; ++frame) {
        missiles.update(kStep, nullptr, party, {}, std::array{MissileStop::of(chest)});
        const auto taken = missiles.takeHits();
        hits.insert(hits.end(), taken.begin(), taken.end());
    }
    REQUIRE(hits.size() >= 2);
    CHECK(hits[0].player == 0);
    CHECK(hits[1].player == 1);
    CHECK(hits[0].damage == 25.0f);
    for (const EnemyMissileHit& hit : hits) {
        CHECK_FALSE(hit.worldContact);
    }
    // A player the bolt stays in is hurt again only after a quarter of a second.
    EnemyMissiles slow;
    EnemyMissileKind crawl = garm;
    crawl.speed = 1.0f;
    slow.launch(crawl, {0, 3, 0}, {0, 3, 60}, 1, nullptr, 5);
    const std::array inside{playerAt({0, 0, 0.5f}, 0)};
    s32 times = 0;
    for (s32 frame = 0; frame < 30; ++frame) { // a second
        slow.update(kStep, nullptr, inside);
        times += static_cast<s32>(slow.takeHits().size());
    }
    CHECK(times == 4);
    CHECK(slow.missile(0).lived == Approx(1.0f).margin(0.01f));
}

TEST_CASE("the brood's death shot is held where it starts for its burst, then flies at twenty a "
          "second through players, walls and items for three seconds",
          "[game][enemies][death-shot]") {
    // StartEnemyDeathFX: velocity 20 (0x803480F0), collision radius and morph time 3
    // (0x803480F8), power 50 (0x803480FC), damage type 0x100020, effect flags 0x8C01 (no
    // world or item collision); the morph flag holds it still until DEATHFX2 takes over.
    EnemyMissileKind kind = EnemyMissileKind::deathShot();
    CHECK(kind.slot == EnemyMissileKind::kDeathShot);
    CHECK(kind.damage == 50.0f);
    CHECK(kind.speed == 20.0f);
    CHECK(kind.radius == 3.0f);
    CHECK(kind.weight == 0.0f);
    CHECK(kind.throughWorld);
    CHECK(kind.pierces());
    CHECK(kind.hitFlags() == 0x100020u);
    kind.held = 0.5f;
    // A floor with a wall across it at z 10, a chest at z 15, players on the corpse, at 20
    // and at 40.
    std::vector<CollisionTriangle> level = floor();
    level.push_back(triangle({-12, 0, 10}, {12, 8, 10}, {12, 0, 10}, {0, 0, -1}));
    level.push_back(triangle({-12, 0, 10}, {-12, 8, 10}, {12, 8, 10}, {0, 0, -1}));
    WorldCollision collision;
    collision.build(level);
    Obstacle chest;
    chest.centre = Vec3{0, 0, 15};
    chest.halfAcross = 2.0f;
    chest.halfAlong = 1.0f;
    chest.height = 6.0f;
    const std::array party{playerAt({0, 0, 2}, 0), playerAt({0, 0, 20}, 1),
                           playerAt({0, 0, 40}, 2)};
    EnemyMissiles missiles;
    missiles.launch(kind, {0, 0, 0}, {0, 0, 1}, 1, nullptr, -1);
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).heldLeft == 0.5f);
    CHECK(missiles.missile(0).secondsLeft == 3.5f);
    std::vector<EnemyMissileHit> hits;
    const auto run = [&](s32 frames) {
        for (s32 frame = 0; frame < frames; ++frame) {
            missiles.update(kStep, &collision, party, {}, std::array{MissileStop::of(chest)});
            const auto taken = missiles.takeHits();
            hits.insert(hits.end(), taken.begin(), taken.end());
        }
    };
    run(15); // half a second: held, striking the one standing in it every quarter second
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).position == Vec3{0, 0, 0});
    CHECK(hits.size() == 2);
    for (const EnemyMissileHit& hit : hits) {
        CHECK(hit.player == 0);
        CHECK(hit.damage == 50.0f);
        CHECK(hit.flags == 0x100020u);
        CHECK_FALSE(hit.worldContact);
    }
    run(15); // through the wall
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).position.z == Approx(10.0f).margin(0.05f));
    run(60); // past the chest and both players
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).position.z == Approx(50.0f).margin(0.05f));
    // Each is hurt as it is reached and again a quarter of a second on, still within it:
    // three units of shot and one of player make eight of overlap, two fifths of a second.
    const auto struck = [&](s32 player) {
        return std::ranges::count_if(
            hits, [&](const EnemyMissileHit& hit) { return hit.player == player; });
    };
    CHECK(struck(1) == 2);
    CHECK(struck(2) == 2);
    CHECK(std::ranges::none_of(hits, [](const EnemyMissileHit& hit) { return hit.worldContact; }));
    run(16); // its three seconds of flight are up
    CHECK(missiles.count() == 0);
}

TEST_CASE("a standing safe rock takes a missile's blow, and stops even the garm's bolt while it "
          "stands",
          "[game][enemies][enemy-missile-items]") {
    Obstacle cover;
    cover.centre = Vec3{0, 0, 10};
    cover.cylinderRadius = 2.0f;
    cover.halfAcross = 2.0f;
    cover.halfAlong = 2.0f;
    cover.height = 6.0f;
    const std::array party{playerAt({0, 0, 20})};
    const auto shoot = [&](const EnemyMissileKind& kind, const MissileStop& rock) {
        EnemyMissiles missiles;
        missiles.launch(kind, {0, 3, 0}, {0, 3, 20}, 1, nullptr, 0);
        std::vector<EnemyMissileHit> hits;
        std::vector<RockHit> blows;
        for (s32 frame = 0; frame < 60 && missiles.count() > 0; ++frame) {
            missiles.update(kStep, nullptr, party, {}, std::array{rock});
            const auto taken = missiles.takeHits();
            hits.insert(hits.end(), taken.begin(), taken.end());
            const auto dealt = missiles.takeRockHits();
            blows.insert(blows.end(), dealt.begin(), dealt.end());
        }
        return std::pair{hits, blows};
    };
    const MissileStop rock{.box = cover, .rock = 3, .rockHealth = 30, .rockArmor = 0};
    const auto [hits, blows] = shoot(EnemyMissileKind::bolt(10, 25, 0.3f), rock);
    REQUIRE(blows.size() == 1);
    CHECK(blows[0].rock == 3);
    CHECK(blows[0].damage == 10.0f);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].worldContact); // it stops there, the player behind untouched

    const EnemyMissileKind garm = *enemyMissileOf(27, EnemyMissileKind::kBolt);
    const auto [held, struck] = shoot(garm, rock);
    CHECK(struck.size() == 1);
    REQUIRE(held.size() == 1);
    CHECK(held[0].player == -1); // a rock left standing stops it
    // One its blow brings down lets it through to the player.
    MissileStop brittle = rock;
    brittle.rockHealth = 5;
    const auto [through, felled] = shoot(garm, brittle);
    // (The level deals each blow before the next frame; left standing here, it is struck again.)
    CHECK_FALSE(felled.empty());
    REQUIRE_FALSE(through.empty());
    CHECK(through[0].player == 0);
}
TEST_CASE("a gas blast's ring tells where it reaches, for the level's food; fire does not",
          "[game][enemies][enemy-gas]") {
    EnemyMissiles missiles;
    EnemyBlast gas;
    gas.position = Vec3{0, 0, 0};
    gas.radius = 7.5f;
    gas.damage = 10.0f;
    gas.flags = EnemyBlast::kGas;
    gas.stages = {1.0f};
    missiles.blast(gas);
    EnemyBlast fire = gas;
    fire.flags = 0;
    missiles.blast(fire);
    std::vector<GasReach> reaches;
    for (s32 frame = 0; frame < 30; ++frame) {
        missiles.update(kStep, nullptr, {});
        const auto taken = missiles.takeGasReaches();
        reaches.insert(reaches.end(), taken.begin(), taken.end());
    }
    REQUIRE(reaches.size() > 2);
    CHECK(reaches.front().damage == Approx(15.0f * (1.0f - 0.33f)).margin(0.5f));
    CHECK(reaches.back().radius > reaches.front().radius);
    CHECK(reaches.back().radius <= 7.5f);
    CHECK(reaches.size() < 30); // only the gas, and it stops two thirds through
    missiles.clear();
    CHECK(missiles.takeGasReaches().empty());
}

TEST_CASE("while the swarm is shrunk what it throws is that size and does half, and what was "
          "thrown before keeps its harm",
          "[game][enemies][shrink]") {
    EnemyMissiles missiles;
    const std::vector<EnemyView> party{playerAt(Vec3{0.0f, 0.0f, 20.0f})};
    const EnemyMissileKind arrow = EnemyMissileKind::arrow();
    missiles.launch(arrow, Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 3.0f, 20.0f}, 1.0f, nullptr, 2);
    missiles.setShrink(0.667f);
    CHECK(missiles.shrink() == Approx(0.667f));
    missiles.launch(arrow, Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 3.0f, 20.0f}, 1.0f, nullptr, 3);
    REQUIRE(missiles.count() == 2);
    CHECK(missiles.missile(0).kind.damage == Approx(arrow.damage));
    CHECK(missiles.missile(0).scale == 1.0f);
    CHECK(missiles.missile(1).kind.damage == Approx(0.5f * arrow.damage));
    CHECK(missiles.missile(1).scale == Approx(0.667f));
    std::vector<EnemyMissileHit> hits;
    for (s32 i = 0; i < 90 && hits.size() < 2; ++i) {
        missiles.update(kStep, nullptr, party);
        for (const EnemyMissileHit& hit : missiles.takeHits()) {
            hits.push_back(hit);
        }
    }
    REQUIRE(hits.size() == 2);
    for (const EnemyMissileHit& hit : hits) {
        CHECK(hit.player == 0);
        CHECK(hit.damage == Approx(hit.shooter == 2 ? arrow.damage : 0.5f * arrow.damage));
    }
    // Whole again, the next throw is whole.
    missiles.setShrink(1.0f);
    missiles.launch(arrow, Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 3.0f, 20.0f}, 1.0f, nullptr, 4);
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).kind.damage == Approx(arrow.damage));
    CHECK(missiles.missile(0).scale == 1.0f);
}
} // namespace
