#include <array>
#include <filesystem>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelFixtures.h"
#include "game/world/LevelCatalog.h"
#include "game/world/StaticScenery.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::filesystem::path sceneryFixture(const std::string& suffix = "") {
    const auto dir = test::sampleLevel("static-scenery-" + suffix);
    writeTextFile(dir / "objects.json",
                  std::string{R"({"objects":[{"index":0,"name":"PROP)"} + suffix +
                      R"(","file":"models/000_WALL.obj","meshTriangles":1}]})");
    test::convertModelFixture(dir);
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"UNRELATED",
      "nodes":[{"name":"ROOT","position":[0,0,0]}]}]})");
    writeTextFile(dir / "world.json", R"({"objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[{"type":10,"subtype":0,"name":"PROP","collisionType":1,
        "radius":1,"height":5,"collisionOffset":[0.25,0,0]}],
      "itemInstances":[{"info":0,"position":[0,2,0],"minPlayers":1}]})");
    return dir;
}

TEST_CASE("ordinary scenery opts into each static model name without weakening tree placement",
          "[static-scenery][item-static-fallback]") {
    const std::string suffix = GENERATE("", "L1", "L1ROOT");
    const auto dir = sceneryFixture(suffix);
    ItemArchive archive;
    REQUIRE(archive.load(dir));
    test::FakeRenderDevice device;
    ItemFigure strict;
    const ItemInstance instance;
    CHECK_FALSE(strict.place(device, archive, "PROP", instance, nullptr));
    CHECK_FALSE(strict.hasFigure());
    CHECK(strict.placeStaticFallback(device, archive, "PROP", instance, nullptr, 0));
    CHECK(strict.hasFigure());
    strict.update(1.0f / 30);
    strict.draw(device, Mat4{1}, WorldLighting{});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].vertices.size() == 3);
    CHECK_FALSE(strict.place(device, archive, "PROP", instance, nullptr));
    CHECK_FALSE(strict.hasFigure());
}

TEST_CASE("static item scenery is grounded, cylindrical and follows its supporting floor",
          "[static-scenery][item-support]") {
    const auto dir = sceneryFixture();
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    ItemArchive archive;
    REQUIRE(archive.load(dir));
    test::FakeRenderDevice device;
    CollisionTriangle floor;
    floor.vertices = {Vec3{-5, 0, -5}, Vec3{0, 0, 5}, Vec3{5, 0, -5}};
    floor.object = 7;
    floor.objectFlags = 0x1004;
    WorldCollision collision;
    collision.build({floor});
    const std::array<s32, 1> moving{7};
    collision.setMovingObjects(moving);
    collision.setObjectTransform(7, Mat4{1});
    StaticScenery props;
    props.bind(device, layout, archive, &collision);
    REQUIRE(props.size() == 1);
    CHECK(props.prop(0).figure.hasFigure());
    CHECK(props.prop(0).figure.position().y == Approx(0.1f));
    CHECK(props.prop(0).support.object() == 7);
    REQUIRE(props.obstacles().size() == 1);
    CHECK(props.obstacles()[0].cylinderRadius == 1);
    CHECK(props.obstacles()[0].centre.x == Approx(0.25f));
    CHECK(props.obstacles()[0].blocksSegment(Vec3{-3, 1, 0}, Vec3{3, 1, 0}, 0.5f));
    props.capturePresentation();
    collision.setObjectTransform(7, glm::translate(Mat4{1}, Vec3{2, -3, 0}));
    props.syncFloors(); // camera cuts carry scenery without running gameplay
    CHECK(props.prop(0).figure.position() == Vec3{2, -2.9f, 0});
    CHECK(props.obstacles()[0].centre.x == Approx(2.25f));
    CHECK(props.obstacles()[0].centre.y == Approx(-2.9f));
    props.setPlayerCount(0);
    CHECK(props.obstacles().empty());
    props.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 1);
    CHECK(device.draws.empty());
    props.setPlayerCount(1);
    props.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 1);
    CHECK(device.draws.size() == 1);
}

TEST_CASE("scenery borrows missing realm meshes but preserves a level's animation tree",
          "[static-scenery][item-static-fallback]") {
    const auto realmDir = sceneryFixture("L1");
    const auto levelDir = test::sampleLevel("static-scenery-level");
    writeTextFile(levelDir / "animations.json", R"({"trees":[{"name":"UNRELATED",
      "nodes":[{"name":"ROOT","position":[0,0,0]}]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(realmDir));
    ItemArchive realm;
    ItemArchive level;
    REQUIRE(realm.load(realmDir));
    REQUIRE(level.load(levelDir));
    test::FakeRenderDevice device;
    StaticScenery props;
    props.bind(device, layout, level, nullptr, &realm);
    REQUIRE(props.size() == 1);
    CHECK(props.prop(0).figure.hasFigure());
    CHECK(props.prop(0).figure.nodeTransform("PROP").has_value());
    props.clear();
    writeTextFile(levelDir / "animations.json", R"({"trees":[{"name":"PROP",
      "nodes":[{"name":"LEVEL_OVERRIDE","object":"WALL","parent":-1,"position":[0,0,0]}]}]})");
    REQUIRE(level.load(levelDir));
    props.bind(device, layout, level, nullptr, &realm);
    REQUIRE(props.size() == 1);
    CHECK(props.prop(0).figure.nodeTransform("LEVEL_OVERRIDE").has_value());
    CHECK_FALSE(props.prop(0).figure.nodeTransform("PROP").has_value());
}

TEST_CASE("static scenery respects no-geometry, no-floor and exact-party authoring",
          "[static-scenery]") {
    const auto dir = sceneryFixture();
    writeTextFile(dir / "world.json", R"({"objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[{"type":10,"subtype":0,"name":"PROP","collisionType":1,
         "collisionFlags":1,"radius":1,"height":5}],
      "itemInstances":[{"info":0,"position":[0,2,0],"flags":2,"minPlayers":12},
         {"info":0,"position":[2,2,0],"params":[41,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    ItemArchive archive;
    REQUIRE(archive.load(dir));
    test::FakeRenderDevice device;
    CollisionTriangle floor;
    floor.vertices = {Vec3{-5, 0, -5}, Vec3{0, 0, 5}, Vec3{5, 0, -5}};
    floor.objectFlags = 4;
    WorldCollision collision;
    collision.build({floor});
    StaticScenery props;
    props.bind(device, layout, archive, &collision);
    REQUIRE(props.size() == 1); // positive subtype overrides belong to other handlers
    CHECK_FALSE(props.prop(0).figure.hasFigure());
    CHECK(props.prop(0).figure.position().y == 2);
    CHECK(props.obstacles().empty());
    props.setPlayerCount(2);
    CHECK(props.obstacles().size() == 1);
    props.setPlayerCount(3);
    CHECK(props.obstacles().empty());
    props.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 1);
    CHECK(device.draws.empty());
}

TEST_CASE("every catalogued native exit sign has a renderable static item model",
          "[static-scenery][exit-signs][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    usize signs = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto level = catalog.byName(name);
            REQUIRE(level);
            WorldLayout layout;
            REQUIRE(layout.load(root / level->directory));
            bool hasSign = false;
            for (const auto& instance : layout.itemInstances()) {
                if (instance.info >= 0 &&
                    static_cast<usize>(instance.info) < layout.itemInfos().size()) {
                    const auto& info = layout.itemInfos()[static_cast<usize>(instance.info)];
                    hasSign |= info.type == 10 && info.subtype == 0;
                }
            }
            if (!hasSign) {
                continue;
            }
            CAPTURE(name);
            ItemArchive archive;
            REQUIRE(archive.load(root / level->items));
            ItemArchive own;
            if (!level->ownItems.empty()) {
                // Like LevelWorld, a missing optional archive (T2) borrows realm art.
                own.load(root / level->ownItems);
            }
            StaticScenery props;
            props.bind(device, layout, own.loaded() ? own : archive, nullptr, &archive);
            REQUIRE(props.size() > 0);
            for (usize i = 0; i < props.size(); ++i) {
                const auto& prop = props.prop(i);
                CAPTURE(prop.instance);
                ++signs;
                CHECK(prop.figure.hasFigure());
                CHECK(prop.box.cylinderRadius == 1);
                CHECK(prop.box.height == 5);
            }
            device.draws.clear();
            props.draw(device, Mat4{1}, {}, nullptr, TreeModel::Pass::All, 1);
            CHECK_FALSE(device.draws.empty());
        }
    }
    CHECK(signs == 41);
}

TEST_CASE("G1 exit sign stops the player through the live fixture update",
          "[static-scenery][exit-signs][fixtures][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    fixtures.bind({device, world, weapons, effects, audio});
    REQUIRE(fixtures.scenery().size() == 1);
    const auto& sign = fixtures.scenery().prop(0);
    CHECK(sign.instance == 107);
    CHECK(sign.figure.hasFigure());
    CHECK(sign.figure.position().x == Approx(118.109375f));
    CHECK(sign.figure.position().z == Approx(-476.6953f));
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, sign.figure.position(), 0);
    players[0].life = PlayerLife::Standing;
    fixtures.update(1, 1.0f / 60, players, {});
    const auto at = players[0].actor.position();
    CHECK(glm::length(Vec2{at.x - sign.box.centre.x, at.z - sign.box.centre.z}) >=
          sign.box.cylinderRadius + players[0].actor.radius() - 0.01f);
    bool blocksEnemyShot = false;
    for (const auto& stop : fixtures.missileStops()) {
        blocksEnemyShot |= stop.box.centre == sign.box.centre;
    }
    CHECK(blocksEnemyShot);
    fixtures.clear();
    CHECK(fixtures.scenery().size() == 0);
}
} // namespace
