#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/Damage.h"
#include "game/world/DestructibleWalls.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Shootable walls own removable geometry, health and party visibility", "[walls]") {
    const auto directory = test::scratchDirectory("destructible-walls");
    writeTextFile(directory / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
      "itemInfos":[{"type":10,"subtype":42,"hitPoints":25,"armor":1}],
      "itemInstances":[{"info":0,"name":"WALL","minPlayers":2,"position":[10,0,5],
       "collision":[{"normal":[0,0,1],"vertices":[[-5,0,0],[5,0,0],[0,10,0]]}]}]})");
    writeTextFile(directory / "objects.json", R"({"objects":[
      {"index":0,"name":"WALL","file":"wall.obj","meshTriangles":1}]})");
    writeTextFile(directory / "wall.obj", "v -5 0 0\nv 5 0 0\nv 0 10 0\nusemtl tex0\nf 1 2 3\n");
    writeFile(directory / "skin.png", test::kTinyPng);
    writeTextFile(directory / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldCollision collision;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    DestructibleWalls walls;
    walls.bind(device, layout, models, textures, collision);
    REQUIRE(walls.size() == 1);
    CHECK_FALSE(walls.standing(0));
    CHECK_FALSE(collision.solid(walls.wall(0).object));
    CHECK_FALSE(walls.strike(0, 100, collision));
    walls.setPlayerCount(2, collision);
    CHECK(walls.standing(0));
    CHECK(collision.resolveWalls({10, 3, 4.8f}, 1, 0, 6).z != Catch::Approx(4.8f));
    walls.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(walls.target(0, 6000).pointNear({10, 3, 0}) == Vec3{10, 3, 5});
    CHECK_FALSE(walls.strike(0, 100, collision, Damage::kGas));
    CHECK(walls.wall(0).health == 25);
    CHECK(walls.strike(0, 10, collision) == 16);
    CHECK(walls.wall(0).flash > 0);
    walls.update(1.0f / 30);
    CHECK(walls.wall(0).flash == 0);
    CHECK(walls.strike(0, 0.5f, collision) == 15); // minimum one after armour
    CHECK(walls.strike(0, 16, collision) == 0);
    CHECK_FALSE(walls.strike(0, 16, collision)); // no duplicate sound/destruction
    CHECK_FALSE(collision.solid(walls.wall(0).object));
    CHECK(collision.resolveWalls({10, 3, 4.8f}, 1, 0, 6).z == Catch::Approx(4.8f));
    device.draws.clear();
    walls.draw(device, Mat4{1}, {});
    CHECK(device.draws.empty());
    walls.setPlayerCount(4, collision);
    CHECK_FALSE(walls.standing(0));
    collision.clear();
    walls.bind(device, layout, models, textures, collision);
    walls.setPlayerCount(2, collision);
    CHECK(walls.wall(0).health == 25);
    CHECK(walls.standing(0));
}

TEST_CASE("Temple's five shootable walls load their level meshes and authored surfaces",
          "[walls][unpacked]") {
    const auto directory = test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path();
    test::unpackedOrSkip("LEVELS/LEVELE1/objects.json");
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldCollision collision;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    REQUIRE(collision.load(directory, layout));
    const usize before = collision.triangleCount();
    DestructibleWalls walls;
    walls.bind(device, layout, models, textures, collision);
    REQUIRE(walls.size() == 5);
    usize added = 0;
    for (usize i = 0; i < walls.size(); ++i) {
        CAPTURE(i);
        CHECK(walls.wall(i).model.bound());
        REQUIRE_FALSE(walls.wall(i).surface.empty());
        added += walls.wall(i).surface.size();
        CHECK(walls.standing(i));
    }
    CHECK(collision.triangleCount() == before + added);
    walls.draw(device, Mat4{1}, {});
    CHECK(device.draws.size() >= 5);
}
} // namespace
