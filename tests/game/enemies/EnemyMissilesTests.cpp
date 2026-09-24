#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    REQUIRE(glm::length(missiles.missile(0).velocity) == Approx(25.0f));
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
    // A shot with nothing in its way ends after its life.
    missiles.launch(EnemyMissileKind::arrow(), Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 4.0f, 100.0f},
                    1.0f, nullptr, 0);
    for (s32 i = 0; i < 200; ++i) {
        missiles.update(kStep, nullptr, nobody);
    }
    REQUIRE(missiles.count() == 0);
    const auto expired = missiles.takeHits();
    REQUIRE(expired.size() == 1);
    CHECK_FALSE(expired[0].worldContact);
    CHECK(expired[0].effect().empty());
    CHECK(expired[0].sound().empty());
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
    intent = suicide.think(bomber, sense);
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
    // The archer: placed at strength four, the archer's body and way, a second-tier's health.
    EnemySpawn spawn;
    spawn.kind = 13;
    spawn.tier = kArcherStrength;
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
    // The bomber lobs.
    spawn.tier = kBomberStrength;
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
    spawn.algorithm = -1;
    spawn.position = Vec3{-30.0f, 0.0f, 0.0f};
    const auto suicide = enemies.spawn(spawn, {});
    REQUIRE(suicide.has_value());
    REQUIRE(enemies.tierOf(*suicide) == 1);
    REQUIRE(enemies.algorithmOf(*suicide) == kSuicideWay);
    const std::vector<EnemyView> near{playerAt(Vec3{-30.0f, 0.0f, 12.0f})};
    std::vector<EnemyBurst> bursts;
    for (s32 i = 0; i < 600 && bursts.empty(); ++i) {
        enemies.update(kTicks, kStep, near, {}, &missiles, 1.0f);
        bursts = enemies.takeBursts();
    }
    REQUIRE(bursts.size() == 1);
    REQUIRE(bursts[0].enemy == *suicide);
    REQUIRE(bursts[0].damage == 25.0f);
    REQUIRE(bursts[0].position.z > 5.0f); // it ran most of the way
    REQUIRE_FALSE(enemies.alive(*suicide));
}

} // namespace
