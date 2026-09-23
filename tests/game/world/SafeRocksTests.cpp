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
    REQUIRE(rocks.rock(0).obstacle.cylinderRadius == Approx(2.3f));

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
    REQUIRE_FALSE(rocks.strike(0, 1000)); // destruction effect is not replayed
    REQUIRE_FALSE(rocks.strike(99, 1000));
    rocks.activate(0);
    REQUIRE(rocks.standing(0));
    REQUIRE(rocks.rock(0).health == 120);
    REQUIRE(rocks.rock(0).tier == 3);
    REQUIRE_FALSE(rocks.strike(0, -20));
    REQUIRE(rocks.rock(0).health == 120);
    rocks.setPlayerCount(2);
    REQUIRE(rocks.obstacles().size() == 2);
    REQUIRE(rocks.rock(1).health == 80);
    rocks.clear();
    REQUIRE(rocks.size() == 0);
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
