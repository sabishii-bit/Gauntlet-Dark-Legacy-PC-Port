#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelTransporters.h"
#include "game/world/LevelWorld.h"

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
            pads.bind(device, layout, world.items(), players);
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
