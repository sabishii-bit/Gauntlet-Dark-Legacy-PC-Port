#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/SafeRocks.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("safe rocks keep three health tiers, leave a ruin and can be reactivated",
          "[game][world][safe-rocks]") {
    const auto dir = test::scratchDirectory("safe-rocks");
    writeTextFile(dir / "world.json", R"({
      "objects": [{"name":"GROUND","position":[0,0,0],"next":-1,"child":-1}],
      "itemInfos": [
        {"type":10,"subtype":41,"name":"ROCK","radius":2.3,"height":5,
         "collisionType":1,"hitPoints":40,"armor":10},
        {"type":10,"subtype":43,"name":"BARREL","hitPoints":5}],
      "itemInstances": [
        {"info":0,"minPlayers":1,"position":[7,30,-1],"rotation":[0,0.4,0],
         "params":[41,0,3,0,0,0,0,0,0,0,0,0]},
        {"info":1,"minPlayers":1,"position":[0,0,0],"rotation":[0,0,0],
         "params":[0,0,3,0,0,0,0,0,0,0,0,0]},
        {"info":0,"minPlayers":2,"position":[20,30,-1],"rotation":[0,0,0],
         "params":[0,0,2,0,0,0,0,0,0,0,0,0]}]
    })");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    test::FakeRenderDevice device;
    ItemArchive missing;
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, layout, missing));
    REQUIRE(rocks.size() == 2); // not the barrel
    rocks.setPlayerCount(1);
    REQUIRE(rocks.standing(0));
    REQUIRE_FALSE(rocks.standing(1));
    REQUIRE_FALSE(rocks.standing(2));
    REQUIRE(rocks.rock(0).position == Vec3{7, 30, -1});
    REQUIRE(rocks.rock(0).health == 120);
    REQUIRE(rocks.rock(0).tier == 3);
    REQUIRE(rocks.obstacles().size() == 1);
    REQUIRE(rocks.attackAnchors().size() == 1);
    REQUIRE(Vec3{rocks.attackAnchors()[0][3]} == Vec3{7, 30, -1});
    REQUIRE(rocks.rock(0).obstacle.cylinderRadius == Approx(2.3f));
    REQUIRE(rocks.blocksBreath(Vec3{7, 32, -10}, Vec3{7, 32, 10}));
    REQUIRE(rocks.blocksSegment(Vec3{9.6f, 32, -10}, Vec3{9.6f, 32, 10}, 0.5f));
    REQUIRE_FALSE(rocks.blocksSegment(Vec3{9.6f, 32, -10}, Vec3{9.6f, 32, 10}, 0.1f));
    REQUIRE_FALSE(rocks.blocksBreath(Vec3{20, 32, -10}, Vec3{20, 32, 10}));

    REQUIRE_FALSE(rocks.strike(0, 50)); // 40 after armour, exactly the tier boundary
    REQUIRE(rocks.rock(0).health == 80);
    REQUIRE(rocks.rock(0).tier == 2);
    REQUIRE_FALSE(rocks.strike(0, 5)); // still takes one
    REQUIRE(rocks.rock(0).health == 79);
    REQUIRE_FALSE(rocks.strike(0, 49));
    REQUIRE(rocks.rock(0).health == 40);
    REQUIRE(rocks.rock(0).tier == 1);
    REQUIRE(rocks.strike(0, 1000));
    REQUIRE(rocks.rock(0).health == 0);
    REQUIRE(rocks.rock(0).tier == 0);
    REQUIRE_FALSE(rocks.standing(0));
    REQUIRE(rocks.obstacles().empty());
    REQUIRE(rocks.attackAnchors().size() == 1); // eruptions still use destroyed cover
    REQUIRE_FALSE(rocks.blocksBreath(Vec3{7, 32, -10}, Vec3{7, 32, 10}));
    REQUIRE_FALSE(rocks.strike(0, 1000)); // destruction effect is not replayed
    REQUIRE_FALSE(rocks.strike(99, 1000));
    rocks.activate(0);
    REQUIRE(rocks.standing(0));
    REQUIRE(rocks.rock(0).health == 120);
    REQUIRE(rocks.rock(0).tier == 3);
    REQUIRE_FALSE(rocks.strike(0, -20));
    REQUIRE(rocks.blocksBreath(Vec3{7, 32, -10}, Vec3{7, 32, 10}));
    REQUIRE(rocks.rock(0).health == 120);
    rocks.setPlayerCount(2);
    REQUIRE(rocks.obstacles().size() == 2);
    REQUIRE(rocks.attackAnchors().size() == 2);
    REQUIRE(rocks.rock(1).health == 80);
    rocks.hideForEruptions();
    REQUIRE(rocks.obstacles().empty());
    REQUIRE(rocks.eruptionTargets().size() == 2);
    REQUIRE(rocks.rock(0).dormant);
    REQUIRE_FALSE(rocks.strike(0, 100));
    rocks.scheduleActivation(0, 1.0f);
    rocks.update(0.9f);
    REQUIRE(rocks.rock(0).dormant);
    // A second eruption may reset the timer before the same rock becomes active.
    rocks.scheduleActivation(0, 0.5f);
    rocks.update(0.4f);
    REQUIRE_FALSE(rocks.standing(0));
    rocks.update(0.11f);
    REQUIRE(rocks.standing(0));
    REQUIRE_FALSE(rocks.rock(0).dormant);
    REQUIRE(rocks.eruptionTargets().size() == 1);
    REQUIRE(rocks.arenaTargets().size() == 2);
    REQUIRE(rocks.arenaTargets()[0].active);
    REQUIRE_FALSE(rocks.arenaTargets()[1].active);
    REQUIRE(rocks.eruptionTargets()[0].index == 1);
    REQUIRE(rocks.rock(0).health == 120);
    REQUIRE(rocks.strike(0, 1000));
    REQUIRE(rocks.eruptionTargets().size() == 2);
    rocks.update(10);
    REQUIRE_FALSE(rocks.standing(0)); // an expired timer cannot reactivate it twice
    rocks.clear();
    REQUIRE(rocks.size() == 0);
}

TEST_CASE("Yeti ice rocks stay invisible until their eruption completes",
          "[game][world][safe-rocks][yeti][unpacked]") {
    const auto root = test::unpackedOrSkip("LEVELS/LEVELI5/world.json").parent_path();
    const auto itemRoot = test::unpackedOrSkip("ITEMS/LEVELI5/objects.json").parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root));
    ItemArchive items;
    REQUIRE(items.load(itemRoot));
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, layout, items));
    rocks.setPlayerCount(1);
    REQUIRE(rocks.size() == 13);
    rocks.hideForEruptions();
    REQUIRE(rocks.eruptionTargets().size() == 8); // five placements require larger parties
    REQUIRE(rocks.obstacles().empty());
    rocks.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.empty());
    rocks.scheduleActivation(3, 35.0f / 30.0f);
    rocks.update(34.0f / 30.0f);
    REQUIRE_FALSE(rocks.standing(3));
    rocks.update(1.01f / 30.0f);
    REQUIRE(rocks.standing(3));
    REQUIRE(rocks.rock(3).health == 90);
    REQUIRE(rocks.obstacles().size() == 1);
    REQUIRE(rocks.eruptionTargets().size() == 7);
    rocks.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE(rocks.strike(3, 1000));
    REQUIRE(rocks.eruptionTargets().size() == 8);
    rocks.clear();
    rocks.update(10); // no timer or borrowed model survives close
    REQUIRE(rocks.eruptionTargets().empty());
}

TEST_CASE("breath cover segments respect shape height radius rotation and endpoints",
          "[game][world][safe-rocks][breath]") {
    Obstacle cover;
    cover.height = 5;
    cover.cylinderRadius = 2;
    REQUIRE(cover.blocksSegment(Vec3{0, 2, -8}, Vec3{0, 2, 8}, 0.5f));
    REQUIRE_FALSE(cover.blocksSegment(Vec3{0, 6, -8}, Vec3{0, 6, 8}, 0.5f));
    REQUIRE_FALSE(cover.blocksSegment(Vec3{0, 2, -8}, Vec3{0, 2, -4}, 0.5f));
    REQUIRE(cover.blocksSegment(Vec3{2.4f, 2, -8}, Vec3{2.4f, 2, 8}, 0.5f));
    REQUIRE_FALSE(cover.blocksSegment(Vec3{2.6f, 2, -8}, Vec3{2.6f, 2, 8}, 0.5f));
    REQUIRE(cover.blocksSegment(Vec3{0, -8, 0}, Vec3{0, 8, 0}, 0.5f));
    cover.cylinderRadius = 0;
    cover.halfAcross = 1;
    cover.halfAlong = 5;
    REQUIRE_FALSE(cover.blocksSegment(Vec3{4, 2, -8}, Vec3{4, 2, 8}, 0.5f));
    cover.yaw = 1.5707963f;
    REQUIRE(cover.blocksSegment(Vec3{4, 2, -8}, Vec3{4, 2, 8}, 0.5f));
    cover.solid = false;
    REQUIRE_FALSE(cover.blocksSegment(Vec3{4, 2, -8}, Vec3{4, 2, 8}, 0.5f));
}

TEST_CASE("round arena cover pushes radially rather than using a square bounding box",
          "[game][world][safe-rocks]") {
    Obstacle rock;
    rock.centre = Vec3{7, 30, -1};
    rock.cylinderRadius = 2.3f;
    rock.height = 5;
    const Vec3 inside = rock.centre + Vec3{1, 0, 1};
    const Vec3 pushed = rock.pushOut(inside, 1);
    REQUIRE(glm::length(pushed - rock.centre) == Approx(3.3f));
    REQUIRE(pushed.x - rock.centre.x == Approx(pushed.z - rock.centre.z));
    REQUIRE(pushed.y == inside.y);
    REQUIRE(rock.touchedBy(pushed, 1));
    const Vec3 corner = rock.centre + Vec3{3, 0, 3};
    REQUIRE(rock.pushOut(corner, 1) == corner);
    REQUIRE_FALSE(rock.touchedBy(corner, 1));
    const Vec3 above = inside + Vec3{0, 6, 0};
    REQUIRE(rock.pushOut(above, 1) == above);
    REQUIRE_FALSE(rock.touchedBy(above, 1));
    REQUIRE(glm::length(rock.pushOut(rock.centre, 1) - rock.centre) == Approx(3.3f));
    rock.solid = false;
    REQUIRE(rock.pushOut(inside, 1) == inside);
}

TEST_CASE("the dragon's lair renders all six safe rocks and every retail damage model",
          "[game][world][safe-rocks][unpacked]") {
    const auto root = test::unpackedOrSkip("LEVELS/LEVELB6/world.json").parent_path();
    const auto itemRoot = test::unpackedOrSkip("ITEMS/LEVELB6/objects.json").parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root));
    ItemArchive items;
    REQUIRE(items.load(itemRoot));
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, layout, items));
    rocks.setPlayerCount(1);
    REQUIRE(rocks.size() == 6);
    REQUIRE(rocks.obstacles().size() == 6);
    for (usize i = 0; i < rocks.size(); ++i) {
        const auto& rock = rocks.rock(i);
        REQUIRE(rock.health == 120);
        REQUIRE(rock.armor == 10);
        REQUIRE(rock.obstacle.cylinderRadius == Approx(2.3f));
        REQUIRE(rock.position ==
                layout.itemInstances()[static_cast<usize>(rock.instance)].position);
        for (const auto& model : rock.models) {
            REQUIRE(model.bound());
        }
    }
    rocks.draw(device, Mat4{1.0f}, {});
    REQUIRE(device.draws.size() >= 6);
    usize intactVertices = 0;
    for (const auto& draw : device.draws) {
        REQUIRE(draw.texture != nullptr);
        intactVertices += draw.vertices.size();
    }
    REQUIRE(intactVertices > 0);
    for (usize i = 0; i < rocks.size(); ++i) {
        REQUIRE(rocks.strike(i, 1000));
    }
    REQUIRE(rocks.obstacles().empty());
    device.draws.clear();
    rocks.draw(device, Mat4{1.0f}, {});
    REQUIRE_FALSE(device.draws.empty()); // tier zero is a real rubble mesh, not deletion
    usize ruinVertices = 0;
    for (const auto& draw : device.draws) {
        ruinVertices += draw.vertices.size();
    }
    REQUIRE(ruinVertices != intactVertices);
}

} // namespace
