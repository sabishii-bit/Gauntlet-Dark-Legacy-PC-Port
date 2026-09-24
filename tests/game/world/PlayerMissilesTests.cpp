#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/world/WorldCollision.h"

#include "game/world/PlayerMissiles.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 60.0f;

TEST_CASE("super shots pierce bodies once and ignore walls", "[game][items][missiles]") {
    PlayerMissiles missiles;
    MissileLaunch launch;
    launch.position = {0, 3, 0};
    launch.velocity = Vec3{0, 0, 30};
    launch.spec = &MissileSpec::superShot();
    launch.flags = 0x100020;
    launch.damage = 10;
    REQUIRE(missiles.launch(launch));
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-50, -10, 10}, Vec3{50, -10, 10}, Vec3{0, 50, 10}};
    WorldCollision collision;
    collision.build({wall});
    const std::array<MissileTarget, 3> targets{
        {{1, {0, 0, 8}, 1, 8}, {2, {0, 0, 16}, 1, 8}, {3, {0, 0, 16}, 1, 8}}};
    for (s32 i = 0; i < 60; ++i) {
        missiles.update(kStep, &collision, targets);
    }
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).position.z == Approx(30));
    const auto hits = missiles.takeImpacts();
    REQUIRE(hits.size() == 3);
    CHECK(hits[0].target == 1);
    CHECK(hits[1].target == 2);
    CHECK(hits[2].target == 3);
    missiles.update(3, &collision, targets);
    CHECK(missiles.count() == 0);
    CHECK(missiles.takeImpacts().empty());
}

TEST_CASE("reflective weapons rebound without creating a damaging wall hit",
          "[game][items][missiles]") {
    PlayerMissiles missiles;
    MissileLaunch launch;
    launch.position = {0, 3, 0};
    launch.velocity = Vec3{0, 0, 30};
    launch.spec = &MissileSpec::of(0);
    launch.flags = 0x200000;
    launch.damage = 10;
    REQUIRE(missiles.launch(launch));
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-50, -10, 10}, Vec3{50, -10, 10}, Vec3{0, 50, 10}};
    WorldCollision collision;
    collision.build({wall});
    missiles.update(0.5f, &collision);
    REQUIRE(missiles.count() == 1);
    CHECK(missiles.missile(0).velocity.z < 0);
    const auto hits = missiles.takeImpacts();
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].damage == 0);
    CHECK(hits[0].effect.empty());
    missiles.update(3, &collision);
    CHECK(missiles.count() == 0);
}

MissileLaunch axeFrom(const Vec3& position) {
    MissileLaunch launch;
    launch.owner = 1;
    launch.position = position;
    launch.direction = Vec3{0.0f, 0.0f, 1.0f};
    launch.speed = 30.0f;
    launch.reach = 15.0f;
    launch.spec = &MissileSpec::of(0);
    return launch;
}

TEST_CASE("each class throws its own weapon, the tier its level earns", "[game][world][missiles]") {
    REQUIRE(MissileSpec::of(0).model == "AXE");
    REQUIRE(MissileSpec::of(0).weight == 12.0f);
    REQUIRE(MissileSpec::of(0).spin > 18.0f);
    REQUIRE_FALSE(MissileSpec::of(0).staysInHand);
    REQUIRE(MissileSpec::of(3).model == "BOW");
    REQUIRE(MissileSpec::of(3).radius == 0.7f);
    REQUIRE(MissileSpec::of(3).spin == 0.0f);
    REQUIRE(MissileSpec::of(3).staysInHand);
    REQUIRE(MissileSpec::of(8).model == "MIN");
    REQUIRE(MissileSpec::of(0).impactTree == "SPARKS");
    REQUIRE(MissileSpec::of(7).impactTree == "EXPSMALL");
    REQUIRE(MissileSpec::of(15).impactTree == "EXPSMALL");
    REQUIRE(MissileSpec::of(16).model == "STF"); // Sumner throws as a wizard
    REQUIRE(MissileSpec::of(-4).model == "AXE");
    bool inCostume = false;
    REQUIRE(MissileSpec::treeName(0, 55, &inCostume) == "AXE_THROW0");
    REQUIRE(inCostume);
    REQUIRE(MissileSpec::treeName(2, 1, &inCostume) == "STF_THROW1");
    REQUIRE_FALSE(inCostume);
    REQUIRE(MissileSpec::treeName(2, 35) == "STF_THROW2");
    REQUIRE(MissileSpec::treeName(2, 99) == "STF_THROW3");
    REQUIRE(MissileSpec::treeName(7, 85) == "BOM_THROW3");
    REQUIRE(MissileSpec::byMagic(2));
    REQUIRE(MissileSpec::byMagic(14));
    REQUIRE_FALSE(MissileSpec::byMagic(0));
}

TEST_CASE("a spread throw fans out fifteen degrees apart, straight on first",
          "[game][world][missiles]") {
    const Vec3 ahead{0.0f, 0.0f, 1.0f};
    REQUIRE(PlayerMissiles::spread(ahead, 1) == std::vector<Vec3>{ahead});
    REQUIRE(PlayerMissiles::spread(ahead, 0).size() == 1);
    const std::vector<Vec3> three = PlayerMissiles::spread(ahead, 3);
    REQUIRE(three.size() == 3);
    REQUIRE(three[0] == ahead);
    REQUIRE(three[1].x == Approx(std::sin(PlayerMissiles::kSpreadStep)));
    REQUIRE(three[2].x == Approx(-std::sin(PlayerMissiles::kSpreadStep)));
    REQUIRE(three[1].z == Approx(std::cos(PlayerMissiles::kSpreadStep)));
    const std::vector<Vec3> five = PlayerMissiles::spread(ahead, 5);
    REQUIRE(five.size() == 5);
    REQUIRE(five[3].x == Approx(0.5f)); // thirty degrees
    REQUIRE(glm::length(five[4]) == Approx(1.0f));
    REQUIRE(PlayerMissiles::spread(ahead, 9).size() == 5);
}

TEST_CASE("strength sets a missile's pace and holding the attack its reach",
          "[game][world][missiles]") {
    REQUIRE(PlayerMissiles::speedFor(0) == 20.0f);
    REQUIRE(PlayerMissiles::speedFor(1000) == 60.0f);
    REQUIRE(PlayerMissiles::speedFor(5000) == 60.0f);
    REQUIRE(PlayerMissiles::speedFor(600) == Approx(44.0f));
    REQUIRE(PlayerMissiles::reachFor(0.2f) == 15.0f);
    REQUIRE(PlayerMissiles::reachFor(0.32f) == Approx(25.0f));
    REQUIRE(PlayerMissiles::reachFor(3.0f) == Approx(35.0f));
}

TEST_CASE("a missile is lobbed to come down half a unit under its start at its reach",
          "[game][world][missiles]") {
    PlayerMissiles missiles;
    REQUIRE_FALSE(missiles.launch(MissileLaunch{}));
    REQUIRE(missiles.launch(axeFrom(Vec3{1.0f, 3.0f, 2.0f})));
    REQUIRE(missiles.count() == 1);
    REQUIRE(missiles.missile(0).owner == 1);
    REQUIRE(missiles.missile(0).velocity.z == 30.0f);
    REQUIRE(missiles.missile(0).velocity.y > 0.0f);
    // Half a second at thirty a second is its reach of fifteen.
    f32 highest = 0.0f;
    for (s32 i = 0; i < 30; ++i) {
        missiles.update(kStep, nullptr);
        highest = std::max(highest, missiles.missile(0).position.y);
    }
    const PlayerMissiles::Missile& axe = missiles.missile(0);
    REQUIRE(axe.position.z == Approx(17.0f).margin(0.01f));
    REQUIRE(axe.position.x == 1.0f);
    REQUIRE(axe.position.y == Approx(2.5f).margin(0.1f));
    REQUIRE(highest > 3.0f);
    REQUIRE(axe.tumble == Approx(18.85f * 0.5f).margin(0.01f));
    // It faces the way it flies and has turned over by its tumble.
    const Mat4 transform = PlayerMissiles::transformOf(axe);
    REQUIRE(Vec3(transform[3]) == axe.position);
    REQUIRE(transform[0].x == Approx(1.0f)); // flying along +z, its side stays its side
    // With nothing to stop it, its time runs out.
    for (s32 i = 0; i < 200; ++i) {
        missiles.update(kStep, nullptr);
    }
    REQUIRE(missiles.count() == 0);
    REQUIRE(missiles.takeImpacts().empty());
}

TEST_CASE("walls and floors stop a missile where it strikes", "[game][world][missiles]") {
    // A wall across z = 10 facing the thrower, and a floor far below.
    CollisionTriangle wall;
    wall.normal = Vec3{0.0f, 0.0f, -1.0f};
    wall.vertices = {Vec3{-20.0f, -5.0f, 10.0f}, Vec3{20.0f, -5.0f, 10.0f},
                     Vec3{0.0f, 40.0f, 10.0f}};
    WorldCollision collision;
    collision.build({wall});
    PlayerMissiles missiles;
    REQUIRE(missiles.launch(axeFrom(Vec3{0.0f, 3.0f, 0.0f})));
    for (s32 i = 0; i < 60 && missiles.count() > 0; ++i) {
        missiles.update(kStep, &collision);
    }
    REQUIRE(missiles.count() == 0);
    const std::vector<MissileImpact> impacts = missiles.takeImpacts();
    REQUIRE(impacts.size() == 1);
    REQUIRE(impacts[0].position.z > 8.0f);
    REQUIRE(impacts[0].position.z < 10.5f);
    REQUIRE(impacts[0].owner == 1);
    REQUIRE(impacts[0].potion == 0);
    REQUIRE(impacts[0].effect == "SPARKS");
    REQUIRE(impacts[0].wallSound == MissileWallSound::Level);
    REQUIRE(missiles.takeImpacts().empty());

    // Over a floor, the lob comes down onto it.
    CollisionTriangle floor;
    floor.normal = Vec3{0.0f, 1.0f, 0.0f};
    floor.vertices = {Vec3{-50.0f, 0.0f, -50.0f}, Vec3{0.0f, 0.0f, 100.0f},
                      Vec3{50.0f, 0.0f, -50.0f}};
    WorldCollision ground;
    ground.build({floor});
    REQUIRE(missiles.launch(axeFrom(Vec3{0.0f, 3.0f, 0.0f})));
    for (s32 i = 0; i < 180 && missiles.count() > 0; ++i) {
        missiles.update(kStep, &ground);
    }
    REQUIRE(missiles.count() == 0);
    const std::vector<MissileImpact> landed = missiles.takeImpacts();
    REQUIRE(landed.size() == 1);
    REQUIRE(landed[0].position.z > 15.0f); // past its reach, the floor lower than its drop
    REQUIRE(landed[0].position.y < 1.5f);
    // A potion flies off at the velocity it is given and says what it was where it lands.
    MissileLaunch toss = axeFrom(Vec3{0.0f, 4.0f, 0.0f});
    toss.spec = &MissileSpec::potion();
    toss.velocity = Vec3{0.0f, 3.5f, 3.5f};
    toss.potion = 4;
    REQUIRE(missiles.launch(toss));
    REQUIRE(missiles.missile(0).velocity == Vec3{0.0f, 3.5f, 3.5f});
    for (s32 i = 0; i < 180 && missiles.count() > 0; ++i) {
        missiles.update(kStep, &ground);
    }
    const std::vector<MissileImpact> burst = missiles.takeImpacts();
    REQUIRE(burst.size() == 1);
    REQUIRE(burst[0].potion == 4);
    REQUIRE(burst[0].position.z > 2.0f);
    REQUIRE(burst[0].position.z < 8.0f);
    missiles.launch(axeFrom(Vec3{0.0f, 3.0f, 0.0f}));
    missiles.clear();
    REQUIRE(missiles.count() == 0);
}

TEST_CASE("world contacts retain the launched weapon's effect and volley sound policy",
          "[game][world][missiles][projectile-impact]") {
    CollisionTriangle wall;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-20, -5, 10}, Vec3{20, -5, 10}, Vec3{0, 40, 10}};
    WorldCollision collision;
    collision.build({wall});
    PlayerMissiles missiles;
    auto launch = axeFrom({0, 3, 0});
    launch.spec = &MissileSpec::of(7);
    launch.wallSound = MissileWallSound::Silent;
    REQUIRE(missiles.launch(launch));
    missiles.update(0.5f, &collision);
    REQUIRE(missiles.count() == 0);
    const auto impacts = missiles.takeImpacts();
    REQUIRE(impacts.size() == 1);
    CHECK(impacts.front().target == -1);
    CHECK(impacts.front().effect == "EXPSMALL");
    CHECK(impacts.front().wallSound == MissileWallSound::Silent);
    missiles.update(0.5f, &collision);
    CHECK(missiles.takeImpacts().empty());
}

TEST_CASE("what stands in a missile's way stops it and learns what hit it",
          "[game][world][missiles]") {
    REQUIRE(PlayerMissiles::damageFor(0) == PlayerMissiles::kLeastDamage);
    REQUIRE(PlayerMissiles::damageFor(1000) == PlayerMissiles::kMostDamage);
    REQUIRE(PlayerMissiles::damageFor(400) == 11.0f);
    REQUIRE(PlayerMissiles::damageFor(5000) == PlayerMissiles::kMostDamage);
    PlayerMissiles missiles;
    MissileLaunch launch = axeFrom(Vec3{0.0f, 2.0f, 0.0f});
    launch.damage = 11.0f;
    REQUIRE(missiles.launch(launch));
    // A barrel ahead and one off to the side, which it passes.
    const std::array<MissileTarget, 2> targets{
        MissileTarget{4, Vec3{6.0f, 0.0f, 5.0f}, 1.0f, 3.0f},
        MissileTarget{9, Vec3{0.0f, 0.0f, 8.0f}, 1.0f, 3.0f}};
    std::vector<MissileImpact> impacts;
    for (s32 i = 0; i < 120 && impacts.empty(); ++i) {
        missiles.update(kStep, nullptr, targets);
        impacts = missiles.takeImpacts();
    }
    REQUIRE(impacts.size() == 1);
    REQUIRE(impacts[0].target == 9);
    REQUIRE(impacts[0].damage == 11.0f);
    REQUIRE(impacts[0].owner == 1);
    REQUIRE(impacts[0].position.z < 8.0f);
    REQUIRE(impacts[0].position.z > 5.5f);
    REQUIRE(missiles.count() == 0);
    // Thrown over a target, it flies on.
    launch.position = Vec3{0.0f, 9.0f, 0.0f};
    REQUIRE(missiles.launch(launch));
    for (s32 i = 0; i < 20; ++i) {
        missiles.update(kStep, nullptr, targets);
    }
    REQUIRE(missiles.takeImpacts().empty());
    REQUIRE(missiles.count() == 1);
}

TEST_CASE("a strong throw's weapon flies twice the size", "[game][world][missiles]") {
    PlayerMissiles missiles;
    MissileLaunch launch = axeFrom(Vec3{0.0f, 2.0f, 0.0f});
    launch.scale = 2.0f;
    REQUIRE(missiles.launch(launch));
    REQUIRE(missiles.missile(0).scale == 2.0f);
    const Mat4 placed = PlayerMissiles::transformOf(missiles.missile(0));
    REQUIRE(glm::length(Vec3{placed[0]}) == Catch::Approx(2.0f));
    launch.scale = 1.0f;
    REQUIRE(missiles.launch(launch));
    REQUIRE(glm::length(Vec3{PlayerMissiles::transformOf(missiles.missile(1))[0]}) ==
            Catch::Approx(1.0f));
}

} // namespace
