#pragma once

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace gdl::test {

/** A level's layout as world.json: a pad that drives a door and fires a second pad driving a
 * bridge, a target on the wall, generators of two kinds and tiers, placed enemies and great
 * ones, pickup records (one named twice, one of no known kind) and an instance with no record. */
inline WorldLayout syntheticLayout() {
    const auto dir = scratchDirectory("synthetic-layout");
    writeTextFile(dir / "world.json", R"({"objects":[
        {"name":"GROUND","position":[0,0,0]},{"name":"E1DOOR","position":[0,0,0]},
        {"name":"E1BRIDGE","position":[0,0,0]},{"name":"CANDLE","position":[0,0,0]},
        {"name":"CANDLE","position":[0,0,0]}],
      "itemInfos":[
        {"type":5,"subtype":0,"name":"BRIDGEPAD"},{"type":5,"subtype":31,"name":"TARGET"},
        {"type":3,"name":"GRU"},{"type":3,"name":"RAT"},{"type":4,"name":"GRU"},
        {"type":4,"name":"GOLEM"},{"type":4,"name":"GAR_EAGL"},
        {"type":1,"subtype":2,"name":"KEY"},{"type":1,"subtype":1,"name":"TREASURE"},
        {"type":1,"subtype":2,"name":"KEY"},{"type":1,"subtype":99,"name":"ODDITY"},
        {"type":4,"name":"RAT"}],
      "itemInstances":[
        {"info":0,"position":[0,0,0],"params":[1,0,0,0,0,0,10,11]},
        {"info":0,"name":"LIFTPAD","position":[0,0,0],"params":[2,0,0,0,0,0,11,0]},
        {"info":1,"position":[0,0,0],"params":[255,255,0,0,0,0,12,0]},
        {"info":2,"position":[0,0,0],"params":[0]},{"info":2,"position":[0,0,0],"params":[3]},
        {"info":3,"position":[0,0,0],"params":[2]},{"info":4,"position":[0,0,0]},
        {"info":4,"position":[0,0,0]},{"info":11,"position":[0,0,0]},
        {"info":5,"position":[0,0,0]},{"info":6,"position":[0,0,0]},
        {"info":7,"position":[0,0,0]},{"info":-1,"position":[0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    return layout;
}

} // namespace gdl::test
