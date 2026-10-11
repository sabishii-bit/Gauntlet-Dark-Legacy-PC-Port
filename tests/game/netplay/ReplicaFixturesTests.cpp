#include <algorithm>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/netplay/CombatPlayback.h"
#include "game/screens/ReplicaFixtures.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

CombatSnapshot blank(u64 tick = 0) {
    CombatSnapshot state;
    state.motion.epoch = state.motion.cameraContinuity = 1;
    state.motion.tick = tick;
    return state;
}
void sameGeometry(std::span<const test::RecordedDraw> actual,
                  std::span<const test::RecordedDraw> expected) {
    REQUIRE_FALSE(expected.empty());
    REQUIRE(actual.size() == expected.size());
    for (usize i = 0; i < actual.size(); ++i) {
        CAPTURE(i);
        const auto& a = actual[i];
        const auto& b = expected[i];
        CHECK(a.texture == b.texture);
        CHECK(a.blend() == b.blend());
        CHECK(a.state.depthWrite == b.state.depthWrite);
        CHECK(a.state.depthTest == b.state.depthTest);
        REQUIRE(a.vertices.size() == b.vertices.size());
        for (usize v = 0; v < a.vertices.size(); ++v) {
            const auto av = a.transform * Vec4{a.vertices[v].position, 1};
            const auto bv = b.transform * Vec4{b.vertices[v].position, 1};
            for (s32 axis = 0; axis < 3; ++axis) {
                CHECK(av[axis] == Approx(bv[axis]).margin(0.0001f));
            }
            CHECK(a.vertices[v].color == b.vertices[v].color);
            CHECK(a.vertices[v].uv.x == Approx(b.vertices[v].uv.x).margin(0.0001f));
            CHECK(a.vertices[v].uv.y == Approx(b.vertices[v].uv.y).margin(0.0001f));
        }
    }
}

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    WorldLayout layout;
    WorldAnimator animator;
    Chests chests;
    LockedGates gates;
    LevelTriggers switches;
    Generators generators;
    Breakables barrels;
    Traps traps;
    SafeRocks rocks;
    Rubble rubble;
    FixtureResources resources;
    explicit Fixture(bool native = false, bool hazards = false) {
        const auto root = test::scratchDirectory("replica-fixtures");
        writeTextFile(root / "body.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"BODY","file":"body.obj"}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "textures.json",
                      R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[{"name":"CHEST",
          "nodes":[{"name":"ROOT","object":"BODY","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"CLOSED","frames":0},
           {"name":"ACTIVE","frames":5,"frameRate":2,
            "tracks":[{"node":0,"flags":32,"frames":[0,4],"values":[0,4]}]},
           {"name":"OPEN","frames":0}]},
          {"name":"GOL_STATUE","nodes":[{"name":"ROOT","object":"BODY","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"IDLE","frames":0},
            {"name":"ACTIVE","frames":5,"frameRate":30,
             "tracks":[{"node":0,"flags":32,"frames":[0,4],"values":[0,4]}]}]}]})");
        writeTextFile(root / "world.json", R"({"objects":[{"name":"FLOOR","position":[0,0,0]}],
          "itemInfos":[{"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":22},
          {"type":7,"subtype":0,"name":"CHEST","radius":1,"height":2},
          {"type":5,"subtype":21,"name":"CHEST","radius":1},
          {"type":5,"subtype":23,"name":"CHEST","radius":1},
          {"type":1,"subtype":1,"name":"CHEST","radius":1}],
          "itemInstances":[{"info":0,"position":[0,0,0],"minPlayers":1,"params":[4,0,0,0]},
           {"info":1,"position":[10,0,0],"minPlayers":1},
           {"info":2,"position":[20,0,0],"minPlayers":2},
           {"info":3,"position":[30,0,0],"minPlayers":1,"flags":2}]})");
        if (hazards) {
            writeTextFile(root / "objects.json", R"({"objects":[
                {"name":"BODY","file":"body.obj"},{"name":"BAREXP0","file":"body.obj"},
                {"name":"BARPOI0","file":"body.obj"},{"name":"CHESTGEXP0","file":"body.obj"},
                {"name":"CHESTSEXP0","file":"body.obj"},{"name":"ITEMEXP0","file":"body.obj"}]})");
            writeTextFile(root / "animations.json", R"({"trees":[{"name":"CHEST",
                "nodes":[{"name":"ROOT","object":"BODY","parent":-1,"position":[0,0,0]}],
                "sequences":[{"name":"OFF","frames":0},
                {"name":"ACTIVE","frames":60,"frameRate":30,
                 "tracks":[{"node":0,"flags":32,"frames":[0,59],"values":[0,4]}]},
                {"name":"DONE","frames":30,"frameRate":30}]},
                {"name":"CHEST_D","nodes":[{"name":"ROOT","object":"BODY","parent":-1,"position":[0,0,0]}],
                 "sequences":[{"name":"OFF","frames":0}]}]})");
            writeTextFile(root / "world.json", R"({"objects":[{"name":"FLOOR","position":[0,0,0]}],
                "itemInfos":[
                {"type":10,"subtype":43,"name":"CHEST","hitPoints":5,"radius":1,"height":2},
                {"type":10,"subtype":44,"name":"CHEST","hitPoints":5,"radius":1,"height":2},
                {"type":10,"subtype":45,"name":"CHEST","hitPoints":5,"radius":1,"height":2},
                {"type":8,"subtype":0,"name":"CHEST","radius":1,"height":2,"value":10,"activeOff":1},
                {"type":8,"subtype":0,"name":"MISSING","radius":1,"height":2,"value":10,"activeOff":1}],
                "itemInstances":[{"info":0,"position":[0,0,0]},
                {"info":1,"position":[10,0,0]},{"info":2,"position":[20,0,0]},
                {"info":0,"position":[30,0,0],"minPlayers":2},
                {"info":3,"position":[40,0,0]},
                {"info":4,"name":"CHEST","position":[50,0,0]}]})");
        }
        test::convertModelFixture(root);
        const auto archiveRoot =
            native ? test::assetOrSkip("ITEMS/LEVELG/ANIM.PS2").parent_path() : root;
        REQUIRE(archive.load(archiveRoot));
        REQUIRE(layout.load(root));
        CHECK(chests.bind(device, layout, archive, nullptr) == !hazards);
        CHECK(gates.bind(device, layout, archive, nullptr) == !hazards);
        if (hazards) {
            REQUIRE(barrels.bind(device, layout, archive, nullptr));
            REQUIRE(traps.bind(device, layout, archive, nullptr));
            barrels.setPlayerCount(1);
            traps.setPlayerCount(1);
        }
        switches.bind(layout, animator, nullptr);
        switches.bindFigures(device, layout, archive);
        switches.setPlayerCount(1);
        REQUIRE(resources.bind(device, std::array{&archive}, generators, rocks));
    }
    CombatSnapshot capture(u64 tick = 0) {
        auto state = blank(tick);
        REQUIRE(FixtureCapture::append(
            state, resources,
            {chests, gates, switches, generators, barrels, traps, rocks, rubble}));
        return state;
    }
};

TEST_CASE("fixture snapshots retain a chest's final opening pose independently of its empty state",
          "[netplay][replica-fixtures]") {
    Fixture f;
    REQUIRE(f.chests.size() == 1);
    const std::array party{ChestVisitor{{0, 0, 0}, 1, 1}};
    REQUIRE_FALSE(f.chests.update(1.0f / 30, party).empty());
    for (usize tick = 0; tick < 12; ++tick) {
        f.chests.update(1.0f / 30, {});
    }
    REQUIRE(f.chests.chest(0).state == Chests::kOpen);
    auto state = f.capture(10);
    REQUIRE_FALSE(state.fixtures.empty());
    const auto& chest = state.fixtures[0];
    CHECK(chest.pose.sequence == 1);
    CHECK(chest.pose.frame == 4);
    CHECK(chest.meshSequence == 2);
    CHECK(chest.meshFrame == 0);
    CHECK(chest.textureSequence == 1);
    CHECK(chest.textureFrame == 4);
    ReplicaFixtures replica;
    REQUIRE(replica.begin(1));
    const auto bytes = CombatPacket::encode(state);
    REQUIRE(bytes);
    const auto decoded = CombatPacket::decode(*bytes);
    REQUIRE(decoded);
    REQUIRE(replica.show(*decoded, f.resources));
    const auto camera = CameraFrame::at({20, 20, -30});
    f.device.draws.clear();
    f.chests.draw(f.device, Mat4{1}, {}, &camera);
    const auto expected = f.device.draws;
    const auto textureCount = f.device.texturesCreated;
    for (s32 repeat = 0; repeat < 3; ++repeat) {
        f.device.draws.clear();
        f.resources.draw(f.device, chest, Mat4{1}, {}, camera, TreeModel::Pass::All);
        sameGeometry(f.device.draws, expected);
    }
    CHECK(f.device.texturesCreated == textureCount);
    CHECK(CombatPacket::encode(f.capture(10)) == bytes);
    f.chests.remove(0);
    REQUIRE(replica.show(f.capture(11), f.resources));
    CHECK(replica.count() + 1 == state.fixtures.size());
    CHECK_FALSE(replica.show(state, f.resources));
    REQUIRE(replica.begin(2));
    CHECK_FALSE(replica.show(state, f.resources));
}

TEST_CASE("fixture snapshots respect switch visibility party gates and resource validation",
          "[netplay][replica-fixtures]") {
    Fixture f;
    const auto first = f.capture();
    CHECK(std::ranges::none_of(first.fixtures,
                               [](const auto& s) { return s.source == FixtureSource::Switch; }));
    f.switches.setPlayerCount(2);
    const auto next = f.capture(1);
    CHECK(std::ranges::count(next.fixtures, FixtureSource::Switch, &FixtureState::source) == 1);
    REQUIRE(f.switches.size() == 2);
    CHECK(f.switches.figure(1) == nullptr); // invisible trigger is never given a replica mesh
    CHECK(f.switches.figure(9999) == nullptr);
    ReplicaFixtures replica;
    REQUIRE(replica.begin(1));
    REQUIRE(replica.show(next, f.resources));
    for (s32 badField = 0; badField < 5; ++badField) {
        auto bad = next;
        auto& value = bad.fixtures[0];
        switch (badField) {
        case 0: value.resource = 65535; break;
        case 1: value.pose.sequence = 65535; break;
        case 2: value.meshSequence = 65535; break;
        case 3: value.textureSequence = 65535; break;
        default: value.meshFrame = 6; break;
        }
        CHECK_FALSE(replica.show(bad, f.resources));
        CHECK(replica.count() == next.fixtures.size());
    }
    auto held = first;
    const FixtureResources empty;
    CHECK_FALSE(FixtureCapture::append(
        held, empty,
        {f.chests, f.gates, f.switches, f.generators, f.barrels, f.traps, f.rocks, f.rubble}));
    CHECK(CombatPacket::encode(held) == CombatPacket::encode(first));
    const std::array visitors{ChestVisitor{{10, 0, 0}, 1, 1}};
    REQUIRE_FALSE(f.gates.update(1, 1.0f / 30, visitors).empty());
    f.gates.update(32, 1, {});
    const auto open = f.capture(2);
    const auto gate = std::ranges::find(open.fixtures, FixtureSource::Gate, &FixtureState::source);
    REQUIRE(gate != open.fixtures.end());
    CHECK(gate->meshSequence == 2);
    CHECK(gate->pose.sequence == 1);
}

TEST_CASE("fixture capture carries chests gates and X-Ray previews on the host floor",
          "[netplay][replica-fixtures]") {
    Fixture f;
    WorldCollision floor;
    const std::array<Vec3, 3> vertices{Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    floor.build({{{0, 1, 0}, vertices, 0, 4100}});
    floor.setMovingObjects(std::array<s32, 1>{0});
    REQUIRE(f.chests.bind(f.device, f.layout, f.archive, &floor));
    REQUIRE(f.gates.bind(f.device, f.layout, f.archive, &floor));
    REQUIRE(f.chests.chest(0).floor);
    const std::array visitors{ChestVisitor{{0, 0, 0}, 1, 1, true}};
    REQUIRE(f.chests.updateXray(f.device, f.archive, f.archive, 0, visitors) == 1);
    const auto first = f.capture(10);
    REQUIRE(first.fixtures.size() == 3);
    CHECK(first.fixtures[0].alpha == Approx(63.0f / 255));
    CHECK(first.fixtures[1].source == FixtureSource::ChestPreview);
    CHECK(glm::length(Vec3{first.fixtures[1].placement[0]}) == Approx(0.65f));
    floor.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, -4, 0}));
    f.chests.syncFloors();
    f.gates.syncFloors();
    const auto second = f.capture(14);
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    for (const auto& state : {first, second}) {
        const auto packets = CombatReplica::packets(state);
        REQUIRE(packets);
        for (const auto& packet : *packets) {
            playback.receive(1, packet);
        }
    }
    const auto middle = playback.sample(12);
    REQUIRE(middle);
    for (usize index = 0; index < second.fixtures.size(); ++index) {
        CHECK(second.fixtures[index].continuity == first.fixtures[index].continuity);
        CHECK(second.fixtures[index].placement[3].y ==
              Approx(first.fixtures[index].placement[3].y - 4));
        CHECK(middle->fixtures[index].placement[3].y ==
              Approx(first.fixtures[index].placement[3].y - 2));
    }
    const auto camera = CameraFrame::at({20, 20, -30});
    f.device.draws.clear();
    f.chests.chest(0).preview.draw(f.device, Mat4{1}, {}, 1, 0.65f, &camera);
    const auto expected = f.device.draws;
    f.device.draws.clear();
    f.resources.draw(f.device, second.fixtures[1], Mat4{1}, {}, camera, TreeModel::Pass::All);
    sameGeometry(f.device.draws, expected);
    f.chests.updateXray(f.device, f.archive, f.archive, 0, {});
    CHECK(f.capture(15).fixtures.size() == 2);
}

TEST_CASE("replicas display host barrel fades and debris without repeating destruction",
          "[netplay][replica-fixtures]") {
    Fixture f(false, true);
    REQUIRE(f.barrels.size() == 4);
    auto initial = f.capture();
    CHECK(std::ranges::count(initial.fixtures, FixtureSource::Barrel, &FixtureState::source) == 3);
    const auto camera = CameraFrame::at({20, 20, -30});
    ReplicaFixtures replica;
    REQUIRE(replica.begin(1));
    for (usize index = 0; index < 3; ++index) {
        const auto hit = f.barrels.strike(index, 100);
        REQUIRE(hit);
        CHECK(hit->broken);
        if (index != 0) {
            REQUIRE(f.rubble.leave(f.device, std::array{&f.archive},
                                   index == 1 ? Rubble::kBlownBarrel : Rubble::kGasBarrel,
                                   f.barrels.transformOf(index)));
        }
    }
    bool faded = false;
    for (u64 tick = 1; tick <= 150; ++tick) {
        f.barrels.update(1.0f / 30);
        auto state = f.capture(tick);
        std::erase_if(state.fixtures,
                      [](const auto& s) { return s.source == FixtureSource::Trap; });
        const auto bytes = CombatPacket::encode(state);
        REQUIRE(bytes);
        const auto decoded = CombatPacket::decode(*bytes);
        REQUIRE(decoded);
        REQUIRE(replica.show(*decoded, f.resources));
        f.device.draws.clear();
        f.barrels.draw(f.device, Mat4{1}, {});
        f.rubble.draw(f.device, Mat4{1}, {});
        const auto expected = f.device.draws;
        const auto loads = f.device.texturesCreated;
        const auto unchanged = CombatPacket::encode(f.capture(tick));
        f.device.draws.clear();
        replica.draw(f.device, f.resources, Mat4{1}, {}, camera);
        sameGeometry(f.device.draws, expected);
        CHECK(f.device.texturesCreated == loads);
        CHECK(CombatPacket::encode(f.capture(tick)) == unchanged);
        for (const auto& fixture : state.fixtures) {
            faded |=
                fixture.source == FixtureSource::Barrel && fixture.alpha > 0 && fixture.alpha < 1;
        }
    }
    CHECK(faded);
    CHECK(f.barrels.barrel(0).state == Breakables::kBroken);
    CHECK_FALSE(f.barrels.barrel(0).gone);
    CHECK(f.barrels.barrel(1).gone);
    CHECK(f.barrels.barrel(2).gone);
    CHECK(f.barrels.barrel(3).health == 5);
    CHECK(f.rubble.size() == 2);
    CHECK(replica.count() == 3);
    const auto late = f.capture(151);
    ReplicaFixtures joined;
    REQUIRE(joined.begin(1));
    REQUIRE(joined.show(late, f.resources));
    f.rubble.clear();
    REQUIRE(joined.show(f.capture(152), f.resources));
    CHECK(joined.count() + 2 == late.fixtures.size());
}

TEST_CASE("trap replicas retain host phases through cycles stops disarming and floor movement",
          "[netplay][replica-fixtures]") {
    Fixture f(false, true);
    WorldCollision floor;
    const std::array<Vec3, 3> vertices{Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
    floor.build({{{0, 1, 0}, vertices, 0, 4100}});
    floor.setMovingObjects(std::array<s32, 1>{0});
    REQUIRE(f.traps.bind(f.device, f.layout, f.archive, &floor));
    REQUIRE(f.barrels.bind(f.device, f.layout, f.archive, &floor));
    f.barrels.setPlayerCount(1);
    const auto first = f.capture(1);
    floor.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, -4, 0}));
    f.traps.syncFloors();
    f.barrels.syncFloors();
    const auto moved = f.capture(2);
    REQUIRE(moved.fixtures.size() == first.fixtures.size());
    for (usize i = 0; i < moved.fixtures.size(); ++i) {
        CHECK(moved.fixtures[i].placement[3].y == Approx(first.fixtures[i].placement[3].y - 4));
    }
    const auto camera = CameraFrame::at({20, 20, -30});
    std::set<s32> phases;
    usize hits = 0;
    const std::array victims{TrapVictim{{40, -3, 0}, 1, 1}};
    for (u64 tick = 3; tick <= 240; ++tick) {
        hits += f.traps.update(2, 1.0f / 30, victims).size();
        phases.insert(f.traps.trap(0).action);
        auto state = f.capture(tick);
        std::erase_if(state.fixtures,
                      [](const auto& s) { return s.source != FixtureSource::Trap; });
        f.device.draws.clear();
        f.traps.draw(f.device, Mat4{1}, {}, &camera);
        const auto expected = f.device.draws;
        const auto image = CombatPacket::encode(f.capture(tick));
        const auto wakes = f.traps.wakes().size();
        ReplicaFixtures replica;
        REQUIRE(replica.begin(1));
        REQUIRE(replica.show(state, f.resources));
        for (s32 repeat = 0; repeat < 2; ++repeat) {
            f.device.draws.clear();
            replica.draw(f.device, f.resources, Mat4{1}, {}, camera);
            sameGeometry(f.device.draws, expected);
        }
        CHECK(CombatPacket::encode(f.capture(tick)) == image);
        CHECK(f.traps.wakes().size() == wakes);
    }
    CHECK(phases.size() == 3);
    CHECK(hits > 0);
    REQUIRE(f.traps.stop(0));
    CHECK(f.traps.trap(0).action == Traps::kResting);
    const auto stopped = f.capture(241);
    REQUIRE(f.traps.disarm(0, f.device, f.layout, f.archive, &floor));
    REQUIRE(f.traps.disarm(1, f.device, f.layout, f.archive, &floor));
    const auto disarmed = f.capture(242);
    CHECK(disarmed.fixtures.size() + 1 == stopped.fixtures.size());
    const auto oldTrap =
        std::ranges::find(stopped.fixtures, FixtureSource::Trap, &FixtureState::source);
    const auto newTrap =
        std::ranges::find(disarmed.fixtures, FixtureSource::Trap, &FixtureState::source);
    REQUIRE(oldTrap != stopped.fixtures.end());
    REQUIRE(newTrap != disarmed.fixtures.end());
    CHECK(oldTrap->resource != newTrap->resource);
    CHECK(newTrap->placement == oldTrap->placement);
    const auto loads = f.device.texturesCreated;
    f.device.draws.clear();
    f.traps.draw(f.device, Mat4{1}, {}, &camera);
    const auto expected = f.device.draws;
    f.device.draws.clear();
    f.resources.draw(f.device, *newTrap, Mat4{1}, {}, camera, TreeModel::Pass::All);
    sameGeometry(f.device.draws, expected);
    CHECK(f.device.texturesCreated == loads);
    CHECK(f.traps.update(600, 10, victims).empty());
}

TEST_CASE("fixture capacity rejects overflow without replacing the last complete checkpoint",
          "[netplay][replica-fixtures]") {
    Fixture f(false, true);
    auto snapshot = f.capture(1);
    const auto prior = CombatPacket::encode(snapshot);
    for (usize i = 0; i < CombatSnapshot::kMaxFixtures; ++i) {
        REQUIRE(f.rubble.leave(f.device, std::array{&f.archive}, Rubble::kItem, Mat4{1}));
    }
    CHECK_FALSE(FixtureCapture::append(
        snapshot, f.resources,
        {f.chests, f.gates, f.switches, f.generators, f.barrels, f.traps, f.rocks, f.rubble}));
    CHECK(CombatPacket::encode(snapshot) == prior);
}

TEST_CASE("native light-trap replicas preserve transparent shutdown frames through repeated rests",
          "[netplay][replica-fixtures][assets]") {
    const auto root = test::assetOrSkip("ITEMS/LEVELA/ANIM.PS2").parent_path().parent_path();
    for (const auto* realm : {"LEVELA", "LEVELC"}) {
        test::FakeRenderDevice device;
        ItemArchive items;
        REQUIRE(items.load(root / realm));
        for (const auto* name : {"FORCEF", "FORCEF_S"}) {
            CAPTURE(realm, name);
            const auto dir = test::scratchDirectory("replica-native-traps");
            writeTextFile(
                dir / "world.json",
                std::string{R"({"objects":[{"name":"GROUND","position":[0,0,0]}],"itemInfos":[
                {"type":8,"subtype":2,"activeOff":1,"name":")"} +
                    name + R"("}],"itemInstances":[{"info":0,"position":[0,0,0]}]})");
            WorldLayout layout;
            REQUIRE(layout.load(dir));
            Traps traps;
            REQUIRE(traps.bind(device, layout, items, nullptr));
            const Generators generators;
            const SafeRocks rocks;
            const Chests chests;
            const LockedGates gates;
            const LevelTriggers switches;
            const Breakables barrels;
            const Rubble rubble;
            FixtureResources resources;
            REQUIRE(resources.bind(device, std::array{&items}, generators, rocks));
            const auto camera = CameraFrame::at({20, 20, -30});
            usize rests = 0;
            for (u64 tick = 0; tick < 210; ++tick) {
                auto state = blank(tick);
                REQUIRE(FixtureCapture::append(
                    state, resources,
                    {chests, gates, switches, generators, barrels, traps, rocks, rubble}));
                REQUIRE(state.fixtures.size() == 1);
                device.draws.clear();
                traps.draw(device, Mat4{1}, {}, &camera);
                const auto expected = device.draws;
                device.draws.clear();
                resources.draw(device, state.fixtures[0], Mat4{1}, {}, camera,
                               TreeModel::Pass::All);
                sameGeometry(device.draws, expected);
                rests += traps.trap(0).action == 0 ? 1 : 0;
                traps.update(2, 1.0f / 30, {});
            }
            CHECK(rests >= 3);
        }
    }
}

TEST_CASE("native chest replicas match the closed opening and retained lid geometry",
          "[netplay][replica-fixtures][assets]") {
    Fixture f(true);
    REQUIRE(f.chests.size() == 1);
    const auto camera = CameraFrame::at({20, 20, -30});
    const auto compare = [&] {
        const auto state = f.capture();
        REQUIRE_FALSE(state.fixtures.empty());
        const auto& chest = state.fixtures[0];
        f.device.draws.clear();
        f.chests.draw(f.device, Mat4{1}, {}, &camera);
        const auto expected = f.device.draws;
        f.device.draws.clear();
        f.resources.draw(f.device, chest, Mat4{1}, {}, camera, TreeModel::Pass::All);
        sameGeometry(f.device.draws, expected);
    };
    compare();
    const std::array party{ChestVisitor{{0, 0, 0}, 1, 1}};
    REQUIRE_FALSE(f.chests.update(1.0f / 30, party).empty());
    for (usize tick = 0; tick < 100; ++tick) {
        f.chests.update(1.0f / 30, {});
        compare();
    }
    CHECK(f.chests.chest(0).state == Chests::kOpen);
}

TEST_CASE("native generator replicas preserve every damaged body and rubble without spawning",
          "[netplay][replica-fixtures][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.level());
    Enemies enemies;
    enemies.open(device, root, &world.collision(), 4, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, world.layout(), enemies, &world.collision(), {}, 4,
                            world.level()->enemies, level->realmId, &world.items()));
    FixtureResources resources;
    const SafeRocks rocks;
    const Breakables barrels;
    const Traps traps;
    const Rubble rubble;
    REQUIRE(resources.bind(device, world.placedItems().archives(), generators, rocks));
    const auto camera = CameraFrame::at({20, 20, -30});
    const Chests chests;
    const LockedGates gates;
    const LevelTriggers switches;
    ReplicaFixtures replica;
    REQUIRE(replica.begin(1));
    usize damageStates = 0;
    for (u64 tick = 0; tick < 4; ++tick) {
        auto state = blank(tick);
        REQUIRE(FixtureCapture::append(
            state, resources,
            {chests, gates, switches, generators, barrels, traps, rocks, rubble}));
        REQUIRE(replica.show(state, resources));
        device.draws.clear();
        generators.draw(device, Mat4{1}, {});
        const auto expected = device.draws;
        const auto count = device.texturesCreated;
        for (usize repeat = 0; repeat < 3; ++repeat) {
            device.draws.clear();
            replica.draw(device, resources, Mat4{1}, {}, camera);
            sameGeometry(device.draws, expected);
        }
        CHECK(device.texturesCreated == count);
        CHECK(enemies.count() == 0);
        for (usize i = 0; i < generators.count(); ++i) {
            const auto id = static_cast<s32>(i);
            CHECK(generators.bredOf(id) == 0);
            CHECK(generators.countdownOf(id) == 0);
            if (generators.standing(id)) {
                const s32 prior = generators.stateOf(id);
                for (s32 hitCount = 0; hitCount < 10000 && generators.stateOf(id) == prior;
                     ++hitCount) {
                    const auto hit = generators.strike(id, 1, 0);
                    if (!hit) {
                        break;
                    }
                    damageStates += hit->stateChanged ? 1 : 0;
                }
            }
        }
    }
    CHECK(damageStates > 0);
}

TEST_CASE("native cover replicas preserve damaged tiers eruptions and borrowed texture clocks",
          "[netplay][replica-fixtures][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    for (const auto* name : {"B6", "I5", "K5"}) {
        CAPTURE(name);
        test::FakeRenderDevice device;
        LevelWorld world;
        const auto level = catalog.byName(name);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        ItemArchive boss;
        if (std::string_view{name} == "K5") {
            REQUIRE(boss.load(root / "MONSTERS/PBOSS"));
        }
        SafeRocks rocks;
        REQUIRE(rocks.bind(device, world.layout(), world.items()));
        rocks.bindAnimations(device, world.items(), std::array{&world.textures(), &boss.textures});
        rocks.setPlayerCount(4);
        const Generators generators;
        const Chests chests;
        const LockedGates gates;
        const LevelTriggers switches;
        const Breakables barrels;
        const Traps traps;
        const Rubble rubble;
        const FixtureCapture::Sources sources{chests,  gates, switches, generators,
                                              barrels, traps, rocks,    rubble};
        // Binding after clock advancement must not add that phase twice on the replica.
        rocks.update(0.4f);
        FixtureResources resources;
        REQUIRE(resources.bind(device, world.placedItems().archives(), generators, rocks));
        const auto camera = CameraFrame::at({20, 20, -30});
        ReplicaFixtures replica;
        REQUIRE(replica.begin(1));
        std::set<const Texture*> phases;
        for (u64 tick = 0; tick < 120; ++tick) {
            rocks.update(1.0f / 30);
            if (tick == 50) {
                rocks.hideForEruptions();
                for (usize i = 0; i < rocks.size(); ++i) {
                    rocks.scheduleActivation(i, 0.2f);
                }
            }
            if (tick == 80) {
                for (usize i = 0; i < rocks.size(); ++i) {
                    rocks.strike(i, 99999);
                }
            }
            auto state = blank(tick);
            REQUIRE(FixtureCapture::append(state, resources, sources));
            REQUIRE(replica.show(state, resources));
            device.draws.clear();
            rocks.draw(device, Mat4{1}, {});
            const auto expected = device.draws;
            for (const auto& draw : expected) {
                phases.insert(draw.texture);
            }
            const auto loads = device.texturesCreated;
            device.draws.clear();
            replica.draw(device, resources, Mat4{1}, {}, camera);
            if (expected.empty()) {
                CHECK(device.draws.empty());
            } else {
                sameGeometry(device.draws, expected);
            }
            if (tick == 50) {
                CHECK(state.fixtures.empty());
            }
            if (tick == 60) {
                CHECK_FALSE(state.fixtures.empty());
            }
            CHECK(device.texturesCreated == loads);
        }
        if (std::string_view{name} == "K5") {
            CHECK(phases.size() >= 15);
        }
    }
}

TEST_CASE("native fixture roster covers every level for four players without running local AI",
          "[netplay][fixture-census][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    std::set<std::string> names{"L1"};
    for (const auto& realm : catalog.realms()) {
        names.insert(realm.levels.begin(), realm.levels.end());
    }
    usize maximum = 0;
    usize expandedMaximum = 0;
    std::string largest;
    for (const auto& name : names) {
        CAPTURE(name);
        const auto level = name == "L1" ? std::optional{LevelRef::tower()} : catalog.byName(name);
        REQUIRE(level);
        test::FakeRenderDevice device;
        LevelWorld world;
        REQUIRE(world.load(device, root, *level));
        world.setPlayerCount(4);
        Enemies enemies;
        enemies.open(device, root, &world.collision(), 4, {}, 1);
        Generators generators;
        const auto* data = world.level();
        REQUIRE(generators.bind(
            device, world.layout(), enemies, &world.collision(), {}, 4,
            data ? std::span<const LevelEnemy>{data->enemies} : std::span<const LevelEnemy>{},
            level->realmId, &world.items(),
            std::array{&world.textures(), &world.items().textures, &world.realmItems().textures}));
        Chests chests;
        LockedGates gates;
        chests.bind(device, world.layout(), world.items(), &world.collision(), &world.realmItems());
        gates.bind(device, world.layout(), world.items(), &world.collision(), &world.realmItems());
        chests.setPlayerCount(4);
        gates.setPlayerCount(4);
        Breakables barrels;
        Traps traps;
        SafeRocks rocks;
        ExitPortals portals;
        const Rubble rubble;
        barrels.bind(device, world.layout(), world.items(), &world.collision(),
                     &world.realmItems());
        traps.bind(device, world.layout(), world.items(), &world.collision(), 1, 1, 1,
                   &world.realmItems());
        rocks.bind(device, world.layout(), world.items());
        portals.bind(device, world.layout(), world.items(), catalog, &world.collision(),
                     &world.realmItems());
        barrels.setPlayerCount(4);
        traps.setPlayerCount(4);
        rocks.setPlayerCount(4);
        FixtureResources resources;
        const auto source = world.placedItems().archives();
        std::vector<ItemArchive*> archives(source.begin(), source.end());
        if (world.realmItems().loaded()) {
            archives.push_back(&world.realmItems());
        }
        REQUIRE(resources.bind(device, archives, generators, rocks));
        auto state = blank();
        REQUIRE(FixtureCapture::append(state, resources,
                                       {chests, gates, world.triggers(), generators, barrels, traps,
                                        rocks, rubble, nullptr, &portals}));
        usize expected = generators.presentation().size();
        for (usize i = 0; i < chests.size(); ++i) {
            expected +=
                chests.chest(i).shown && chests.chest(i).figure.presentation().drawable ? 1 : 0;
        }
        for (usize i = 0; i < gates.size(); ++i) {
            expected += gates.gate(i).shown && gates.gate(i).figure.presentation().drawable ? 1 : 0;
        }
        for (usize i = 0; i < world.triggers().size(); ++i) {
            const auto* figure = world.triggers().figure(i);
            expected += world.triggers().trigger(i).enabled && figure != nullptr &&
                                figure->presentation().drawable
                            ? 1
                            : 0;
        }
        for (usize i = 0; i < barrels.size(); ++i) {
            expected +=
                barrels.barrel(i).shown && barrels.barrel(i).figure.presentation().drawable ? 1 : 0;
        }
        for (usize i = 0; i < traps.size(); ++i) {
            expected += traps.trap(i).shown && traps.trap(i).figure.presentation().drawable ? 1 : 0;
        }
        for (usize i = 0; i < rocks.size(); ++i) {
            const auto& rock = rocks.rock(i);
            expected +=
                rock.shown && !rock.dormant && rock.models[static_cast<usize>(rock.tier)].bound()
                    ? 1
                    : 0;
        }
        for (usize i = 0; i < portals.size(); ++i) {
            const auto& portal = portals.portal(i);
            const bool drawable =
                portal.secret ? portal.icon.presentation().drawable : portal.model.bound();
            expected += drawable ? 1 : 0;
        }
        CHECK(state.fixtures.size() == expected);
        // Loose pickups add rubble; chest previews and exploding barrel debris
        // can overlap their original bodies. Conservatively reserve a statue for
        // every enemy placement, including those that actually start alive.
        const auto possibleStatues =
            std::ranges::count_if(world.layout().itemInstances(), [&](const ItemInstance& item) {
                return item.info >= 0 &&
                       static_cast<usize>(item.info) < world.layout().itemInfos().size() &&
                       world.layout().itemInfos()[static_cast<usize>(item.info)].type ==
                           ItemInfo::kPlacedEnemy;
            });
        expandedMaximum =
            std::max(expandedMaximum, expected + world.placedItems().size() + chests.size() +
                                          barrels.size() + static_cast<usize>(possibleStatues));
        REQUIRE(CombatReplica::packets(state));
        CHECK(enemies.count() == 0);
        if (state.fixtures.size() > maximum) {
            maximum = state.fixtures.size();
            largest = name;
        }
    }
    WARN("Fixture census: " << names.size() << " levels; largest roster " << largest << " with "
                            << maximum);
    CHECK(maximum > 0);
    CHECK(maximum < CombatSnapshot::kMaxFixtures);
    WARN("Fixture census including pickup debris, chest previews and statue reserve: "
         << expandedMaximum);
    CHECK(expandedMaximum < CombatSnapshot::kMaxFixtures);
}
TEST_CASE("native portal replicas retain locked exits raised loops fades and consumed secret icons",
          "[netplay][replica-portals][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize secrets = 0;
    usize locked = 0;
    for (const auto* name : {"L1", "G1", "B2"}) {
        CAPTURE(name);
        test::FakeRenderDevice device;
        LevelWorld world;
        const auto level = std::string_view{name} == "L1" ? std::optional{LevelRef::tower()}
                                                          : catalog.byName(name);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        const TowerAccess access(std::array{CharacterSave{}});
        ExitPortals portals;
        REQUIRE(portals.bind(device, world.layout(), world.items(), catalog, &world.collision(),
                             &world.realmItems(), level->isTower() ? &access : nullptr));
        usize selected = portals.size();
        for (usize i = 0; i < portals.size(); ++i) {
            const auto& portal = portals.portal(i);
            secrets += portal.secret ? 1 : 0;
            locked += portal.shut ? 1 : 0;
            if (!portal.secret && !portal.shut) {
                selected = i;
            }
        }
        REQUIRE(selected < portals.size());
        const Generators generators;
        const SafeRocks rocks;
        const Chests chests;
        const LockedGates gates;
        const LevelTriggers switches;
        const Breakables barrels;
        const Traps traps;
        const Rubble rubble;
        FixtureResources resources;
        std::vector<ItemArchive*> archives{&world.items()};
        if (world.realmItems().loaded()) {
            archives.push_back(&world.realmItems());
        }
        REQUIRE(resources.bind(device, archives, generators, rocks));
        const FixtureCapture::Sources sources{chests, gates, switches, generators, barrels,
                                              traps,  rocks, rubble,   nullptr,    &portals};
        ReplicaFixtures replica;
        REQUIRE(replica.begin(1));
        const auto camera = CameraFrame::at({20, 20, -30});
        const Vec3 position = portals.portal(selected).position;
        const std::array split{PortalVisitor{position, 0.75f, 0, true},
                               PortalVisitor{position + Vec3{1000, 0, 1000}, 0.75f, 1, true}};
        std::set<s32> phases;
        u64 waitingGeneration = 0;
        bool wrapped = false;
        for (u64 tick = 0; tick < 440; ++tick) {
            const auto& portal = portals.portal(selected);
            phases.insert(portal.action);
            if (portal.action == ExitPortals::kWaiting) {
                wrapped |=
                    waitingGeneration != 0 && waitingGeneration != portal.player.generation();
                waitingGeneration = portal.player.generation();
            }
            if (tick == 180 || tick == 200 || tick == 220) {
                portals.setAlpha(portal.tag, static_cast<f32>(tick - 180) / 40);
            }
            if (tick == 300) {
                for (usize i = 0; i < portals.size(); ++i) {
                    if (portals.portal(i).secret) {
                        portals.consume(i);
                    }
                }
            }
            auto state = blank(tick);
            REQUIRE(FixtureCapture::append(state, resources, sources));
            if (tick >= 300) {
                CHECK(std::ranges::none_of(state.fixtures, [](const auto& fixture) {
                    return fixture.source == FixtureSource::SecretPortal;
                }));
            }
            if (tick % 10 == 0) {
                const auto bytes = CombatPacket::encode(state);
                REQUIRE(bytes);
                const auto decoded = CombatPacket::decode(*bytes);
                REQUIRE(decoded);
                REQUIRE(replica.show(*decoded, resources));
                const auto loads = device.texturesCreated;
                for (auto pass : {TreeModel::Pass::Opaque, TreeModel::Pass::Blended}) {
                    device.draws.clear();
                    portals.draw(device, Mat4{1}, {}, &camera, pass);
                    const auto expected = device.draws;
                    device.draws.clear();
                    replica.draw(device, resources, Mat4{1}, {}, camera, pass);
                    if (expected.empty()) {
                        CHECK(device.draws.empty());
                    } else {
                        sameGeometry(device.draws, expected);
                    }
                }
                CHECK(device.texturesCreated == loads);
                auto repeated = blank(tick);
                REQUIRE(FixtureCapture::append(repeated, resources, sources));
                CHECK(CombatPacket::encode(repeated) == bytes);
            }
            REQUIRE_FALSE(portals.update(
                2, 1.0f / 30, tick < 240 ? std::span(split) : std::span<const PortalVisitor>{}));
        }
        CHECK(phases == std::set<s32>{0, 1, 2, 3, 4});
        CHECK(wrapped);
        auto final = blank(440);
        REQUIRE(FixtureCapture::append(final, resources, sources));
        REQUIRE(replica.begin(2));
        CHECK_FALSE(replica.show(final, resources));
        final.motion.epoch = 2;
        REQUIRE(replica.show(final, resources));
        portals.clear();
    }
    CHECK(locked > 0);
    CHECK(secrets > 0);
}

TEST_CASE("statue replication survives list compaction without moving or resurrecting siblings",
          "[netplay][replica-statues]") {
    Fixture f;
    CritterStatues statues;
    CritterStatues::Placement placement;
    placement.kind = CombatantKind::Golem;
    for (s32 i = 0; i < 3; ++i) {
        placement.instance.position = {static_cast<f32>(i * 12), 0, 0};
        REQUIRE(statues.add(f.device, f.archive, placement, nullptr));
    }
    const FixtureCapture::Sources sources{f.chests, f.gates, f.switches, f.generators, f.barrels,
                                          f.traps,  f.rocks, f.rubble,   &statues};
    REQUIRE(f.resources.bind(f.device, std::array{&f.archive}, f.generators, f.rocks, &statues));
    auto first = blank();
    REQUIRE(FixtureCapture::append(first, f.resources, sources));
    const auto lastId = statues.instanceOf(2);
    CHECK(lastId == 3);
    statues.wake(0);
    statues.update(2, 1.0f / 30, {});
    REQUIRE(statues.rising(0));
    auto rising = blank(1);
    REQUIRE(FixtureCapture::append(rising, f.resources, sources));
    CHECK(rising.fixtures.back().pose.generation == first.fixtures.back().pose.generation);
    statues.update(20, 1, {});
    REQUIRE(statues.count() == 2);
    CHECK(statues.instanceOf(0) == 2);
    CHECK(statues.instanceOf(1) == lastId);
    auto after = blank(2);
    REQUIRE(FixtureCapture::append(after, f.resources, sources));
    CHECK(std::ranges::count(after.fixtures, FixtureSource::Statue, &FixtureState::source) == 2);
    CombatPlayback playback;
    REQUIRE(playback.begin(1, 1));
    for (const auto* state : {&first, &after}) {
        const auto packets = CombatReplica::packets(*state);
        REQUIRE(packets);
        for (const auto& packet : *packets) {
            playback.receive(1, packet);
        }
    }
    const auto halfway = playback.sample(1);
    REQUIRE(halfway);
    for (const auto& shown : halfway->fixtures) {
        if (shown.source == FixtureSource::Statue) {
            const auto original =
                std::ranges::find(first.fixtures, shown.key(), &FixtureState::key);
            REQUIRE(original != first.fixtures.end());
            CHECK(shown.placement == original->placement);
        }
    }
    const auto arrived = playback.sample(2);
    REQUIRE(arrived);
    CHECK_FALSE(std::ranges::any_of(arrived->fixtures, [](const auto& shown) {
        return shown.source == FixtureSource::Statue && shown.instance == 1;
    }));
    const auto saved = CombatPacket::encode(after);
    CHECK_FALSE(FixtureCapture::append(after, {}, sources));
    CHECK(CombatPacket::encode(after) == saved);
    statues.clear();
    REQUIRE(statues.add(f.device, f.archive, placement, nullptr));
    CHECK(statues.instanceOf(0) > lastId);
}

TEST_CASE("native statue replicas match idle and rising poses without waking their host",
          "[netplay][replica-statues][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    for (const auto* name : {"GOLEM/LEVELG", "GAR_EAGL", "GAR_LION", "GAR_SERP", "DEATH"}) {
        CAPTURE(name);
        Fixture f;
        ItemArchive archive;
        REQUIRE(archive.load(root / "MONSTERS" / name));
        CritterStatues statues;
        CritterStatues::Placement placement;
        placement.instance.position = {2, 3, 4};
        placement.kind = std::string_view{name}.starts_with("GOLEM") ? CombatantKind::Golem
                                                                     : CombatantKind::Gargoyle;
        if (std::string_view{name} == "DEATH") {
            placement.enemy = EnemySpawn{.kind = kDeathKind, .tier = 1};
        }
        REQUIRE(statues.add(f.device, archive, placement, nullptr));
        if (placement.enemy) {
            placement.enemy->tier = 2;
            placement.instance.position.x = 15;
            REQUIRE(statues.add(f.device, archive, placement, nullptr));
        }
        REQUIRE(
            f.resources.bind(f.device, std::array{&f.archive}, f.generators, f.rocks, &statues));
        const FixtureCapture::Sources sources{f.chests,     f.gates,   f.switches,
                                              f.generators, f.barrels, f.traps,
                                              f.rocks,      f.rubble,  &statues};
        const auto camera = CameraFrame::at({20, 25, -30});
        for (u64 tick = 0; tick < 300 && statues.count() > 0; ++tick) {
            if (tick == 4) {
                for (usize i = 0; i < statues.count(); ++i) {
                    statues.wake(i);
                }
            }
            statues.update(2, 1.0f / 30, {});
            auto state = blank(tick);
            REQUIRE(FixtureCapture::append(state, f.resources, sources));
            std::erase_if(state.fixtures,
                          [](const auto& shown) { return shown.source != FixtureSource::Statue; });
            const auto encoded = CombatPacket::encode(state);
            REQUIRE(encoded);
            const auto decoded = CombatPacket::decode(*encoded);
            REQUIRE(decoded);
            ReplicaFixtures replica;
            REQUIRE(replica.begin(1));
            REQUIRE(replica.show(*decoded, f.resources));
            const usize textures = f.device.texturesCreated;
            f.device.draws.clear();
            statues.draw(f.device, Mat4{1}, {}, &camera);
            const auto expected = f.device.draws;
            f.device.draws.clear();
            replica.draw(f.device, f.resources, Mat4{1}, {}, camera);
            if (expected.empty()) {
                CHECK(f.device.draws.empty());
            } else {
                sameGeometry(f.device.draws, expected);
            }
            CHECK(f.device.texturesCreated == textures);
            if (tick < 4) {
                CHECK_FALSE(statues.woken(0));
                CHECK_FALSE(statues.rising(0));
            }
        }
        CHECK(statues.count() == 0);
        CHECK_FALSE(statues.takeRisen().empty());
    }
}
} // namespace
