#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/Breakables.h"
#include "game/world/Chests.h"
#include "game/world/ExitPortals.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelTransporters.h"
#include "game/world/LevelWorld.h"
#include "game/world/LockedGates.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("transporters link ids rather than instance order and reject missing partners",
          "[transporters]") {
    const auto dir = test::scratchDirectory("transporters-links");
    writeTextFile(dir / "world.json", R"({"objects": [{"name":"GROUND","position":[0,0,0]}],
      "itemInfos": [{"type":11,"name":"TRANS","radius":1.6,"height":2}, {"type":9}],
      "itemInstances": [
        {"info":0,"position":[0,0,10],"params":[2,0,0,0,1,0,0,0]},
        {"info":1,"position":[0,0,10]},
        {"info":0,"position":[0,0,20],"params":[1,0,0,0,2,0,0,0]},
        {"info":0,"position":[40,0,20],"params":[3,0,0,0,3,0,0,0]},
        {"info":0,"minPlayers":2,"position":[0,0,0],"params":[4,0,0,0,1,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    test::FakeRenderDevice device;
    ItemArchive items;
    LevelTransporters pads;
    pads.bind(device, layout, items, 1);
    REQUIRE(pads.size() == 3);
    CHECK(pads.pad(0).destination == 1);
    CHECK(pads.pad(1).destination == 0);
    CHECK_FALSE(pads.pad(2).destination); // a self-id must not link to itself
    CHECK(pads.contact(Vec3{0, 0.2f, 10}, 0.75f, 5) == 0);
    CHECK_FALSE(pads.contact(Vec3{0, 5, 10}, 0.75f, 5));
    CHECK_FALSE(pads.contact(Vec3{4, 0, 10}, 0.75f, 5));
    WorldCollision collision;
    CHECK_FALSE(pads.landing(0, 0.75f, collision, {}, {}));
    CollisionTriangle triangle;
    triangle.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    triangle.normal = Vec3{0, 1, 0};
    collision.build({triangle});
    CHECK(pads.landing(0, 0.75f, collision, {}, {}) == Vec3{0, 0, 20});
    CHECK_FALSE(pads.landing(2, 0.75f, collision, {}, {}));
    CHECK_FALSE(pads.landing(99, 0.75f, collision, {}, {}));
    WorldCamera away;
    away.position.z = 30;
    CHECK_FALSE(pads.landing(0, 0.75f, collision, away, {}));
    pads.bind(device, layout, items, 2);
    CHECK(pads.size() == 4);
    pads.clear();
    CHECK(pads.size() == 0);
}

TEST_CASE("a transporter destination must fit within the shared camera", "[transporters]") {
    CHECK(LevelTransporters::visible(Vec3{0, 0, 20}, 1, {}, {}));
    CHECK_FALSE(LevelTransporters::visible(Vec3{0, 0, -20}, 1, {}, {}));
    CHECK_FALSE(LevelTransporters::visible(Vec3{20, 0, 20}, 1, {}, {}));
    CHECK_FALSE(LevelTransporters::visible(Vec3{0, 20, 20}, 1, {}, {}));
    CHECK_FALSE(LevelTransporters::visible(Vec3{0, 0, 0.5f}, 1, {}, {}));
    CHECK_FALSE(LevelTransporters::visible(Vec3{0, 0, 3000}, 1, {}, {}));
    CHECK(LevelTransporters::soundForRealm(1) == "S_TRANSPORTA");
    CHECK(LevelTransporters::soundForRealm(12) == "S_TRANSPORTS3");
    CHECK(LevelTransporters::soundForRealm(-1).empty());
    CHECK(LevelTransporters::soundForRealm(13).empty());
}

TEST_CASE("placed item art falls back per tree without replacing level-specific figures",
          "[transporters][portals][realm-art]") {
    const bool override = GENERATE(false, true);
    const auto root = test::scratchDirectory("portal-realm-art");
    const auto writeArchive = [](const std::filesystem::path& directory, bool hasTrees, bool wide) {
        std::filesystem::create_directories(directory);
        writeTextFile(directory / "body.obj",
                      wide
                          ? "v 0 0 0\nv 3 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n"
                          : "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(directory / "objects.json", R"({"objects":[
            {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
        writeFile(directory / "skin.png", test::kTinyPng);
        writeTextFile(directory / "textures.json", R"({"bitmaps":[
            {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        writeTextFile(directory / "animations.json", hasTrees
                                                         ? R"({"trees":[
            {"name":"TRANS","nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
             "sequences":[{"name":"ACTIVE","frames":20,"rate":30}]},
            {"name":"EXIT_PORTAL","nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":20,"rate":30}]},
            {"name":"SECRET_ICON","nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":20,"rate":30}]},
            {"name":"CHEST","nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":20,"rate":30}]},
            {"name":"GATEB","nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":20,"rate":30}]},
            {"name":"BAROBJ","nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":20,"rate":30}]}]})"
                                                         : R"({"trees":[{"name":"OTHER","nodes":[
                         {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
                         "sequences":[{"name":"IDLE","frames":20,"rate":30}]}]})");
    };
    writeArchive(root / "own", override, true);
    writeArchive(root / "realm", true, false);
    writeTextFile(root / "world.json", R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
        "itemInfos":[{"type":11,"name":"TRANS"},{"type":9,"name":"EXIT_PORTAL"},
                     {"type":9,"subtype":50,"name":"SECRET_ICON"},
                     {"type":2,"subtype":46,"name":"CHEST"},
                     {"type":7,"name":"GATEB"},
                     {"type":10,"subtype":43,"name":"BAROBJ","hitPoints":5},
                     {"type":1,"name":"SECRET_ICON"}],
        "itemInstances":[{"info":0,"position":[0,0,0]},
                         {"info":1,"position":[10,0,0]},
                         {"info":2,"position":[20,0,0]},
                         {"info":3,"position":[30,0,0],"params":[6,0,0,0,1,0]},
                         {"info":4,"position":[40,0,0]},
                         {"info":5,"position":[50,0,0]}]})");
    test::FakeRenderDevice device;
    ItemArchive own;
    ItemArchive realm;
    WorldLayout layout;
    const LevelCatalog catalog;
    REQUIRE(own.load(root / "own"));
    REQUIRE(realm.load(root / "realm"));
    REQUIRE(layout.load(root));
    LevelTransporters pads;
    pads.bind(device, layout, own, 1, &realm);
    REQUIRE(pads.size() == 1);
    REQUIRE(pads.pad(0).model.bound());
    CHECK(pads.pad(0).animation.playing());
    pads.animate(1.0f / 30);
    CHECK(pads.pad(0).animation.frame() > 0);
    pads.draw(device, Mat4{1}, {});
    ExitPortals exits;
    REQUIRE(exits.bind(device, layout, own, catalog, nullptr, &realm));
    REQUIRE(exits.size() == 2);
    CHECK(exits.portal(0).model.bound());
    CHECK(exits.portal(1).icon.hasFigure());
    exits.draw(device, Mat4{1}, {});
    Chests chests;
    LockedGates gates;
    Breakables barrels;
    REQUIRE(chests.bind(device, layout, own, nullptr, &realm));
    REQUIRE(gates.bind(device, layout, own, nullptr, &realm));
    REQUIRE(barrels.bind(device, layout, own, nullptr, &realm));
    REQUIRE(chests.size() == 1);
    REQUIRE(gates.size() == 1);
    REQUIRE(barrels.size() == 1);
    CHECK(chests.chest(0).figure.hasFigure());
    CHECK(gates.gate(0).figure.hasFigure());
    CHECK(barrels.barrel(0).figure.hasFigure());
    chests.setPlayerCount(1);
    gates.setPlayerCount(1);
    barrels.setPlayerCount(1);
    chests.draw(device, Mat4{1}, {});
    gates.draw(device, Mat4{1}, {});
    barrels.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 6);
    const f32 width = override ? 3.0f : 1.0f;
    for (usize i = 0; i < device.draws.size(); ++i) {
        REQUIRE(device.draws[i].vertices.size() == 3);
        CHECK(device.draws[i].vertices[1].position.x == static_cast<f32>(i) * 10 + width);
    }
    ItemArchive powerups;
    const std::array visitors{ChestVisitor{Vec3{33, 0, 0}, 0.75f, 0, true}};
    REQUIRE(chests.updateXray(device, own, powerups, 0, visitors, &realm) == 1);
    CHECK(chests.chest(0).revealed);
    CHECK(chests.chest(0).preview.hasFigure());
    // The borrowed realm remains loaded and reusable after releasing all consumers.
    exits.clear();
    pads.clear();
    chests.clear();
    gates.clear();
    barrels.clear();
    CHECK(realm.loaded());
}

TEST_CASE("every catalogued transporter has a partner, a landing floor and animated art",
          "[transporters][unpacked]") {
    const s32 players = GENERATE(1, 2, 3, 4);
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELA1/world.json").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize count = 0;
    usize levels = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref);
            WorldLayout layout;
            const auto directory =
                test::unpackedOrSkip(ref->directory + "/world.json").parent_path();
            REQUIRE(layout.load(directory));
            bool hasPads = false;
            for (const auto& info : layout.itemInfos()) {
                hasPads |= info.type == LevelTransporters::kItemType;
            }
            if (!hasPads) {
                continue;
            }
            CAPTURE(name);
            test::FakeRenderDevice device;
            test::unpackedOrSkip(ref->items + "/animations.json");
            test::unpackedOrSkip(ref->directory + "/objects.json");
            test::unpackedOrSkip(ref->directory + "/collision.json");
            LevelWorld world;
            REQUIRE(world.load(device, root, *ref));
            LevelTransporters pads;
            pads.bind(device, layout, world.items(), players, &world.realmItems());
            for (usize i = 0; i < pads.size(); ++i) {
                const auto& pad = pads.pad(i);
                CAPTURE(i, pad.id, pad.destinationId);
                REQUIRE(pad.destination);
                CHECK(pads.pad(*pad.destination).id == pad.destinationId);
                CHECK(world.collision().floorAt(pad.position, 4, 10).has_value());
                CHECK(pad.model.bound());
                CHECK(pad.animation.playing());
                const f32 before = pad.animation.frame();
                pads.animate(1.0f / 30);
                CHECK(pad.animation.frame() != before);
            }
            count += pads.size();
            ++levels;
        }
    }
    CHECK(levels == 7);
    CHECK(count == 36);
}
} // namespace
