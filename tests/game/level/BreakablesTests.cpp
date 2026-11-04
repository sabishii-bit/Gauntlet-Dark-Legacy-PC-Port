#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "game/level/Breakables.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A level of six barrels: a plain one, two holding a random key or gold (the second only for
 * a party of two), one that blows up, one of poison gas and one no blow can harm. */
struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    Breakables barrels;

    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0],"flags":4100}],
          "itemInfos":[
            {"type":10,"subtype":43,"name":"BARREL","radius":1,"height":3,"armor":1,"hitPoints":5},
            {"type":2,"subtype":43,"name":"BARREL","radius":1,"height":3,"armor":1,"hitPoints":5},
            {"type":10,"subtype":44,"name":"BARREL","radius":1,"height":3,"armor":1,"hitPoints":5},
            {"type":10,"subtype":45,"name":"BARREL","radius":1,"height":3,"armor":1,"hitPoints":5},
            {"type":10,"subtype":43,"name":"BARREL","radius":1,"height":3,"armor":-1,"hitPoints":5},
            {"type":-1,"choices":[8,7]},
            {"type":1,"subtype":2,"name":"KEY","value":1},
            {"type":1,"subtype":1,"name":"GOLD","value":200},
            {"type":-1,"choices":[6,7]}],
          "itemInstances":[
            {"info":0,"position":[0,0,0]},
            {"info":1,"position":[20,0,0],"params":[5,0,0,0,3,0,0,0,0,0,0,0]},
            {"info":2,"position":[40,0,0]},
            {"info":3,"position":[60,0,0]},
            {"info":4,"position":[80,0,0]},
            {"info":1,"position":[100,0,0],"minPlayers":2,
             "params":[5,0,0,0,2,0,0,0,0,0,0,0]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"BARREL","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"WHOLE","frames":2,"rate":30},
             {"name":"ACTIVE","frames":45,"rate":30,
              "tracks":[{"node":0,"flags":2,"frames":[0,44],"values":[0,1.3]}]},
             {"name":"DONE","frames":0}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(items.load(directory));
        REQUIRE(barrels.bind(device, layout, AssetScope(items), nullptr));
        barrels.setPlayerCount(2);
        REQUIRE(barrels.size() == 6);
    }
    /** The blow struck on `tick` of the script, if any. */
    std::optional<BreakableStrike> strike(s32 tick) {
        switch (tick) {
        case 0: return barrels.strike(4, 9999);
        case 1: return barrels.strike(0, 0.1f);
        case 2: return barrels.strike(1, 99);
        case 3: return barrels.strike(2, 99);
        case 4: return barrels.strike(3, 99);
        case 5: return barrels.strike(0, 99);
        case 6: barrels.changeContents(5, 6); break;
        case 7: barrels.changeContents(5, 5); return barrels.strike(5, 99);
        default: break;
        }
        return std::nullopt;
    }
};

TEST_CASE("struck barrels give native random drops fade when they blow and leave their staves",
          "[game][level][breakables]") {
    Fixture fixture("barrel-behaviour");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto uploads = fixture.device.texturesCreated;
    bool faded = false;
    bool retainedStaves = false;
    usize drops = 0;
    usize retirements = 0;
    for (s32 tick = 0; tick < 180; ++tick) {
        CAPTURE(tick, seconds);
        fixture.barrels.capturePresentation();
        const auto strike = fixture.strike(tick);
        const auto retired = fixture.barrels.update(seconds);
        if (strike && strike->kind == BreakableStrike::Kind::Holding && strike->broken) {
            REQUIRE(strike->contents >= 6);
            REQUIRE(strike->contents <= 7);
            ++drops;
        }
        retirements += retired.size();
        const f32 opacity = fixture.barrels.opacityOf(2);
        faded |= opacity > 0 && opacity < 1;
        const auto& plain = fixture.barrels.barrel(0);
        retainedStaves |= plain.state == Breakables::kBroken && !plain.gone;
        REQUIRE(fixture.barrels.barrel(4).health == 5);
        if (tick == 1) {
            REQUIRE(plain.health == 4);
        }
    }
    CHECK(faded);
    CHECK(retainedStaves);
    CHECK(drops == 2);
    CHECK(retirements == 2);
    CHECK(fixture.barrels.simulation().seed != Breakables::kSeedStart);
    CHECK(fixture.device.texturesCreated == uploads);
}
} // namespace
