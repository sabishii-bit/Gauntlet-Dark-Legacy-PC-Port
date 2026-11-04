#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "game/level/Chests.h"
#include "game/level/LockedGates.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A level of three gates: one shut across the way, one more for parties of two, and a
 * floor-only one that bars nobody. */
struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    LockedGates gates;

    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0],"flags":4100}],
          "itemInfos":[
            {"type":7,"subtype":1,"name":"GATE","radius":1,"height":3},
            {"type":7,"subtype":2,"name":"GATE","radius":3,"height":1,"collisionType":4}],
          "itemInstances":[
            {"info":0,"position":[0,0,0]},
            {"info":0,"position":[20,0,0],"minPlayers":2},
            {"info":1,"position":[40,0,0],"collision":[
              {"normal":[0,1,0],"vertices":[[-3,0,-3],[3,0,-3],[0,0,3]]}]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[
          {"name":"GATE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"OFF","frames":2,"rate":30},
             {"name":"ACTIVE","frames":45,"rate":30,
              "tracks":[{"node":0,"flags":2,"frames":[0,44],"values":[0,1.3]}]},
             {"name":"OPEN","frames":0}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(items.load(directory));
        REQUIRE(gates.bind(device, layout, AssetScope(items), nullptr));
        gates.setPlayerCount(2);
        REQUIRE(gates.size() == 3);
        REQUIRE_FALSE(gates.gate(2).blocksPassage);
    }
};

TEST_CASE("gates refuse and open in visitor order and bar nobody while they swing",
          "[game][level][locked-gates]") {
    Fixture fixture("gate-behaviour");
    const s32 ticks = GENERATE(1, 2);
    const f32 seconds = static_cast<f32>(ticks) / 60;
    const auto uploads = fixture.device.texturesCreated;
    bool refused = false;
    bool unlocked = false;
    bool passingWhileOpening = false;
    bool opened = false;
    for (s32 tick = 0; tick < 180; ++tick) {
        // Sparse party positions still yield native visitor indices; the second
        // visitor opens gate zero while the first approaches the other gate.
        const std::array party{ChestVisitor{Vec3{20, 0, 0}, 1, tick < 8 ? 0 : 1},
                               ChestVisitor{Vec3{0}, 1, tick < 4 ? 0 : 1},
                               ChestVisitor{Vec3{40, 0, 0}, 1, 1}};
        fixture.gates.capturePresentation();
        const auto events = fixture.gates.update(ticks, seconds, party);
        for (const auto& event : events) {
            refused |= event.kind == GateEvent::Kind::Refused;
            unlocked |= event.kind == GateEvent::Kind::Unlocked;
            CHECK(event.gate != 2);
        }
        const auto& first = fixture.gates.gate(0);
        passingWhileOpening |= first.state == LockedGates::kOpening && !first.box.solid;
        opened |= first.state == LockedGates::kOpen;
        REQUIRE(fixture.gates.gate(2).state == LockedGates::kShut);
    }
    CHECK(refused);
    CHECK(unlocked);
    CHECK(passingWhileOpening);
    CHECK(opened);
    CHECK(fixture.device.texturesCreated == uploads);
}
} // namespace
