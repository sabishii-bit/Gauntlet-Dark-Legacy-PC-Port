#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/CombatCover.h"
#include "game/level/SafeRockCover.h"
#include "game/level/SafeRocks.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_base_of_v<CombatCover, SafeRockCover>);
static_assert(std::is_final_v<SafeRockCover>);

constexpr s32 kSweepSteps = 24;   ///< probes across the rock's side
constexpr f32 kSweepStart = 9.0f; ///< inside the rock, 2 from its centre
constexpr f32 kSweepStep = 0.05f; ///< out to 0.9 past its side
constexpr f32 kPastSide = 9.6f;   ///< 0.3 past its side: within breath's width, not an area's
constexpr f32 kClear = 20.0f;     ///< nowhere near it

/** A breath or area line through the arena at `x`, crossing the rock's height. */
struct Line {
    Vec3 from;
    Vec3 to;
};
Line lineAt(f32 x) {
    return {Vec3{x, 32, -10}, Vec3{x, 32, 10}};
}

TEST_CASE("safe rock cover shelters by the rocks as they stand, breath wider than an area",
          "[game][combat-cover][safe-rock-cover]") {
    const auto dir = test::scratchDirectory("safe-rock-cover");
    writeTextFile(dir / "world.json", R"({
      "objects": [{"name":"GROUND","position":[0,0,0],"next":-1,"child":-1}],
      "itemInfos": [
        {"type":10,"subtype":41,"name":"ROCK","radius":2.3,"height":5,
         "collisionType":1,"hitPoints":40,"armor":10}],
      "itemInstances": [
        {"info":0,"minPlayers":1,"position":[7,30,-1],"rotation":[0,0.4,0],
         "params":[41,0,3,0,0,0,0,0,0,0,0,0]}]
    })");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    test::FakeRenderDevice device;
    ItemArchive missing;
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, layout, missing));
    rocks.setPlayerCount(1);
    REQUIRE(rocks.standing(0));
    const SafeRockCover rockCover{rocks};
    const CombatCover& cover = rockCover;

    const Line through = lineAt(7.0f);
    const Line past = lineAt(kPastSide);
    const Line clear = lineAt(kClear);
    CHECK(cover.blocksBreath(through.from, through.to));
    CHECK(cover.blocksArea(through.from, through.to));
    CHECK(cover.blocksBreath(past.from, past.to));
    CHECK_FALSE(cover.blocksArea(past.from, past.to));
    CHECK_FALSE(cover.blocksBreath(clear.from, clear.to));
    CHECK_FALSE(cover.blocksArea(clear.from, clear.to));
    for (s32 step = 0; step <= kSweepSteps; ++step) {
        const Line line = lineAt(kSweepStart + kSweepStep * static_cast<f32>(step));
        CAPTURE(step);
        CHECK(cover.blocksBreath(line.from, line.to) == rocks.blocksBreath(line.from, line.to));
        CHECK(cover.blocksArea(line.from, line.to) ==
              rocks.blocksSegment(line.from, line.to, CombatCover::kAreaProbeRadius));
    }

    REQUIRE(rocks.strike(0, 1000));
    CHECK_FALSE(cover.blocksBreath(through.from, through.to));
    CHECK_FALSE(cover.blocksArea(through.from, through.to));
    rocks.activate(0);
    CHECK(cover.blocksBreath(through.from, through.to));
    CHECK(cover.blocksArea(through.from, through.to));
}
} // namespace
