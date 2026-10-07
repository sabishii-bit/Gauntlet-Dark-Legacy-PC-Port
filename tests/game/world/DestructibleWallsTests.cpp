#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/combat/Damage.h"
#include "game/world/DestructibleWalls.h"
#include "game/world/TargetAssist.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Shootable walls own removable geometry, health and party visibility",
          "[walls][alpha-wall-fx]") {
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
    test::convertModelFixture(directory);
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
    CHECK(walls.wall(0).collisionCentre == Vec3{10, 1, 5});

    // AddItemSub adds one before fn_8005A404 rotates the offset. A near-vertical
    // offset instead keeps world axes even when the item's mesh is tilted.
    writeTextFile(directory / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
      "itemInfos":[
      {"type":10,"subtype":42,"name":"WALL","collisionOffset":[1,2,3],"hitPoints":25},
      {"type":10,"subtype":42,"name":"WALL","collisionOffset":[0,2,0],"hitPoints":25},
      {"type":10,"subtype":42,"name":"WALL","collisionOffset":[0.01,2,0],"hitPoints":25}],
      "itemInstances":[
      {"info":0,"position":[10,0,5],"rotation":[0,0,1.5707963267948966],
       "collision":[{"normal":[0,0,1],"vertices":[[-5,0,0],[5,0,0],[0,10,0]]}]},
      {"info":1,"position":[10,0,5],"rotation":[0,0,1.5707963267948966],
       "collision":[{"normal":[0,0,1],"vertices":[[-5,0,0],[5,0,0],[0,10,0]]}]},
      {"info":2,"position":[10,0,5],"rotation":[0,0,1.5707963267948966],
       "collision":[{"normal":[0,0,1],"vertices":[[-5,0,0],[5,0,0],[0,10,0]]}]}]})");
    REQUIRE(layout.load(directory));
    collision.clear();
    walls.bind(device, layout, models, textures, collision);
    REQUIRE(walls.size() == 3);
    CHECK(walls.wall(0).collisionCentre.x == Catch::Approx(7));
    CHECK(walls.wall(0).collisionCentre.y == Catch::Approx(1));
    CHECK(walls.wall(0).collisionCentre.z == Catch::Approx(8));
    CHECK(walls.wall(1).collisionCentre == Vec3{10, 3, 5});
    // The native comparison promotes the summed float to double; float(.01) is below .01.
    CHECK(walls.wall(2).collisionCentre == Vec3{10.01f, 3, 5});
}

TEST_CASE("Temple's five shootable walls load their level meshes and authored surfaces",
          "[walls][alpha-aim-acquisition][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path();
    test::assetOrSkip("LEVELS/LEVELE1/objects.ngc");
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
        const auto target = walls.target(i, static_cast<s32>(i));
        REQUIRE(target.acquisition);
        const auto instance = static_cast<usize>(walls.wall(i).object) - layout.objects().size();
        const auto& info =
            layout.itemInfos().at(static_cast<usize>(layout.itemInstances().at(instance).info));
        CHECK(target.acquisition->point == walls.wall(i).collisionCentre);
        CHECK(target.acquisition->radius == std::min(info.radius, 5.0f));
        CHECK(target.acquisition->distanceScale == Catch::Approx(1.2f));
        CHECK(target.acquisition->maxHeight == 2 * info.height);
    }
    CHECK(collision.triangleCount() == before + added);
    walls.draw(device, Mat4{1}, {});
    CHECK(device.draws.size() >= 5);
}

TEST_CASE("invulnerable walls retain collision but are excluded from assisted acquisition",
          "[walls][alpha-target-eligibility]") {
    const auto directory = test::scratchDirectory("invulnerable-wall-aim");
    writeTextFile(directory / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
      "itemInfos":[{"type":10,"subtype":42,"hitPoints":25,"armor":-1,
                    "radius":3,"height":6,"collisionOffset":[0,2,0]}],
      "itemInstances":[{"info":0,"position":[0,0,3],"name":"WALL",
       "collision":[{"normal":[0,0,-1],"vertices":[[-3,0,0],[3,0,0],[0,6,0]]}]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    test::FakeRenderDevice device;
    ModelSet noModels;
    TextureSet noTextures;
    WorldCollision collision;
    DestructibleWalls walls;
    walls.bind(device, layout, noModels, noTextures, collision);
    REQUIRE(walls.size() == 1);
    const auto target = walls.target(0, 6000);
    CHECK_FALSE(TargetAssist::select({0, 3, 0}, {0, 0, 1}, std::span{&target, 1}, 30));
    CHECK_FALSE(TargetAssist::ahead({}, 6, {0, 0, 1}, std::span{&target, 1}, 4, 30));
    CHECK(TargetAssist::around({}, 6, std::span{&target, 1}, 4));
    CHECK(target.touches({0, 3, 3}, 0.25f));
    CHECK(collision.solid(walls.wall(0).object));
    CHECK_FALSE(walls.strike(0, 1000, collision));
    CHECK(walls.standing(0));
}

TEST_CASE("G4 breakaway meshes retain their baked lighting after a hit",
          "[walls][alpha-wall-lighting][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELG4/WORLDS.PS2").parent_path();
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldCollision collision;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    REQUIRE(collision.load(directory, layout));
    DestructibleWalls walls;
    walls.bind(device, layout, models, textures, collision);
    REQUIRE(walls.size() == 13);
    for (usize i = 0; i < walls.size(); ++i) {
        const auto& wall = walls.wall(i);
        const auto instance = static_cast<usize>(wall.object) - layout.objects().size();
        const auto& name = layout.itemInstances().at(instance).name;
        CAPTURE(name);
        const auto found = models.find(name);
        REQUIRE(found);
        const auto& mesh = models.mesh(*found);
        REQUIRE(mesh.prelit);
        const auto checkColors = [&](bool flash) {
            device.draws.clear();
            wall.model.draw(device, Mat4{1}, wall.transform, {});
            REQUIRE(device.draws.size() == mesh.parts.size());
            for (usize p = 0; p < mesh.parts.size(); ++p) {
                const auto& part = mesh.parts[p];
                REQUIRE(device.draws[p].vertices.size() == part.indices.size());
                for (usize v = 0; v < part.indices.size(); ++v) {
                    CHECK(device.draws[p].vertices[v].color ==
                          (flash ? Color::white() : mesh.vertices[part.indices[v]].color));
                }
            }
        };
        checkColors(false);
        REQUIRE(walls.strike(i, 1, collision));
        checkColors(true);
        walls.update(1.0f / 30);
        checkColors(false);
    }
}
} // namespace
