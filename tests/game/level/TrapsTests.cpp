#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/level/Traps.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A level of two traps resting at random between strikes: spikes that spray particles and
 * have a disarmed figure, and a bare blade without one. */
struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive items;
    WorldLayout layout;
    Traps traps;
    Fixture() {
        const auto root = test::scratchDirectory("traps-behaviour");
        writeTextFile(root / "tri.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(
            root / "objects.json",
            R"({"objects":[{"index":0,"name":"TRI","file":"tri.obj","meshTriangles":1}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(
            root / "textures.json",
            R"({"bitmaps":[{"index":0,"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({
          "particles":[{"enables":16928,"texture":"WHITE","particleLife":[0.2,0.1],
            "rate":[30,30,30,30],"flags":128,"flagMask":128}],
          "trees":[
          {"name":"SPIKES","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
            {"name":"SPRAY","parent":0,"position":[0,1,0],"particle":0}],
           "sequences":[{"name":"IDLE","frames":1,"frameRate":30},
                        {"name":"ONA","frames":4,"frameRate":30},
                        {"name":"ONB","frames":4,"frameRate":30}]},
          {"name":"SPIKES_D","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
            {"name":"SPRAY","parent":0,"position":[0,1,0],"particle":0}],
           "sequences":[{"name":"IDLE","frames":2,"frameRate":30}]},
          {"name":"BARE","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"IDLE","frames":1,"frameRate":30},
                        {"name":"ONA","frames":4,"frameRate":30},
                        {"name":"ONB","frames":4,"frameRate":30}]}]})");
        writeTextFile(root / "world.json",
                      R"({"objects":[{"name":"TRI","position":[0,0,0],"flags":4100}],"itemInfos":[
          {"type":8,"subtype":0,"name":"SPIKES","radius":2,"height":3,"value":20,"activeOff":-4},
          {"type":8,"subtype":3,"name":"BARE","radius":2,"height":3,"value":10,"activeOff":-3}],
          "itemInstances":[{"info":0,"position":[0,0,0]},{"info":1,"position":[20,0,0]}]})");
        test::convertModelFixture(root);
        REQUIRE(items.load(root));
        REQUIRE(layout.load(root));
        REQUIRE(traps.bind(device, layout, AssetScope(items), nullptr, 7123, 1.25f, 1.5f));
    }
};

TEST_CASE("traps strike and wake through randomized native cycles at either rate",
          "[game][level][traps]") {
    const auto rate = GENERATE(30, 60);
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    const s32 ticks = 60 / rate;
    Fixture fixture;
    const std::array<TrapVictim, 2> victims{{{{0, 1, 0}, 0.75f, 1.5f}, {{20, 1, 0}, 0.75f, 1.5f}}};
    for (s32 tick = 0; tick < 17; ++tick) {
        fixture.traps.update(ticks, seconds, victims);
    }
    usize hits = 0;
    usize wakes = 0;
    for (s32 tick = 0; tick < 400; ++tick) {
        fixture.traps.capturePresentation();
        const bool held = tick >= 100 && tick < 120;
        hits += fixture.traps.update(ticks, seconds, victims, held).size();
        wakes += fixture.traps.wakes().size();
        if (tick == 200) {
            (void)fixture.traps.stop(0);
        }
    }
    CHECK(hits > 0);
    CHECK(wakes > 0);
}
} // namespace
