#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/world/WorldCollision.h"

#include "game/world/PlayerMissiles.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 60.0f;

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

TEST_CASE("each class throws its own weapon, the tier its level earns",
          "[game][world][missiles]") {
    REQUIRE(MissileSpec::of(0).model == "AXE");
    REQUIRE(MissileSpec::of(0).weight == 12.0f);
    REQUIRE(MissileSpec::of(0).spin > 18.0f);
    REQUIRE_FALSE(MissileSpec::of(0).staysInHand);
    REQUIRE(MissileSpec::of(3).model == "BOW");
    REQUIRE(MissileSpec::of(3).radius == 0.7f);
    REQUIRE(MissileSpec::of(3).spin == 0.0f);
    REQUIRE(MissileSpec::of(3).staysInHand);
    REQUIRE(MissileSpec::of(8).model == "MIN");
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
    for (int i = 0; i < 30; ++i) {
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
    for (int i = 0; i < 200; ++i) {
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
    for (int i = 0; i < 60 && missiles.count() > 0; ++i) {
        missiles.update(kStep, &collision);
    }
    REQUIRE(missiles.count() == 0);
    const std::vector<Vec3> impacts = missiles.takeImpacts();
    REQUIRE(impacts.size() == 1);
    REQUIRE(impacts[0].z > 8.0f);
    REQUIRE(impacts[0].z < 10.5f);
    REQUIRE(missiles.takeImpacts().empty());

    // Over a floor, the lob comes down onto it.
    CollisionTriangle floor;
    floor.normal = Vec3{0.0f, 1.0f, 0.0f};
    floor.vertices = {Vec3{-50.0f, 0.0f, -50.0f}, Vec3{0.0f, 0.0f, 100.0f},
                      Vec3{50.0f, 0.0f, -50.0f}};
    WorldCollision ground;
    ground.build({floor});
    REQUIRE(missiles.launch(axeFrom(Vec3{0.0f, 3.0f, 0.0f})));
    for (int i = 0; i < 180 && missiles.count() > 0; ++i) {
        missiles.update(kStep, &ground);
    }
    REQUIRE(missiles.count() == 0);
    const std::vector<Vec3> landed = missiles.takeImpacts();
    REQUIRE(landed.size() == 1);
    REQUIRE(landed[0].z > 15.0f); // past its reach, where the floor is lower than its drop
    REQUIRE(landed[0].y < 1.5f);
    missiles.launch(axeFrom(Vec3{0.0f, 3.0f, 0.0f}));
    missiles.clear();
    REQUIRE(missiles.count() == 0);
}

} // namespace
