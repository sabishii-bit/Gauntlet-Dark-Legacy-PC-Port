#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/players/Progression.h"
#include "game/world/ExitPortals.h"
#include "game/world/TowerAccess.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** A level with one exit to "g1" at (10, 0, 10) and one to nowhere, and the realm it names. */
std::filesystem::path sampleLevel(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "wdata");
    writeTextFile(dir / "wdata/TOWN.json", R"({"realm": 7, "prefix": "levelG", "levels": [
      {"name": "G1", "title": "Fields"}]})");
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "GROUND", "position": [0, 0, 0], "next": -1, "child": -1}],
  "animations": [], "particles": [], "locators": [],
  "itemInfos": [
    {"type": 9, "subtype": 0, "name": "EXIT_PORTAL", "radius": 3, "height": 2},
    {"type": 1, "subtype": 2, "name": "KEY", "radius": 0.5, "height": 2}],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [10, 0, 10], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 103, 49, 0, 0, 0, 0, 0, 0]},
    {"info": 1, "minPlayers": 1, "position": [0, 0, 0], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 0, "minPlayers": 1, "position": [50, 0, 50], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 122, 57, 0, 0, 0, 0, 0, 0]}]
})");
    return dir;
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    LevelCatalog catalog;
    ItemArchive items; ///< left unloaded: the portals work unseen
    ExitPortals portals;

    explicit Fixture(std::string_view name) {
        const auto dir = sampleLevel(name);
        REQUIRE(layout.load(dir));
        REQUIRE(catalog.load(dir));
        REQUIRE(portals.bind(device, layout, items, catalog, nullptr));
    }

    /** Steps until a portal is left by or `frames` have gone; the portal, if any. */
    std::optional<usize> run(std::span<const PortalVisitor> party, s32 frames) {
        for (s32 i = 0; i < frames; ++i) {
            if (const auto left = portals.update(2, 1.0f / 30.0f, party); left.has_value()) {
                return left;
            }
        }
        return std::nullopt;
    }
};

TEST_CASE("a level's exit items become portals that know where they lead",
          "[game][world][portals]") {
    Fixture f("portals-bind");
    REQUIRE(f.portals.size() == 2);
    const ExitPortals::Portal& fields = f.portals.portal(0);
    REQUIRE(fields.instance == 0);
    REQUIRE(fields.tag == "g1");
    REQUIRE(fields.destination.has_value());
    REQUIRE(fields.destination->name == "G1");
    REQUIRE(fields.radius == 3.0f);
    REQUIRE(fields.action == 0);
    REQUIRE(f.portals.portal(1).tag == "z9");
    REQUIRE_FALSE(f.portals.portal(1).destination.has_value());
    f.portals.setAlpha("g1", 0.5f);
    REQUIRE(f.portals.portal(0).alpha == 0.5f);
    REQUIRE(f.portals.portal(1).alpha == 1.0f);
    REQUIRE_FALSE(f.portals.portal(0).shut);
    f.portals.setAlpha("g1", -1);
    REQUIRE(f.portals.portal(0).alpha == 0);
    f.portals.setAlpha("g1", 2);
    REQUIRE(f.portals.portal(0).alpha == 1);
    f.portals.clear();
    REQUIRE(f.portals.size() == 0);
}

TEST_CASE("exit flame stays on while a visitor walks around a usable portal",
          "[game][world][portals]") {
    Fixture f("portals-flame");
    std::array<PortalVisitor, 1> party{{{.position = Vec3{10, 0, 10}, .party = 0, .still = false}}};
    REQUIRE(f.portals.flamePosition(party));
    for (s32 frame = 0; frame < 10; ++frame) {
        party[0].position.x += 0.1f;
        party[0].still = frame % 2 == 0;
        f.portals.update(2, 1.0f / 30.0f, party);
        REQUIRE(f.portals.flamePosition(party));
        CHECK(*f.portals.flamePosition(party) == Vec3{10, 0, 10});
    }
    party[0].still = true;
    REQUIRE(f.portals.flamePosition(party));
    CHECK(*f.portals.flamePosition(party) == Vec3{10, 0, 10});
    party[0].position.y = 30;
    CHECK_FALSE(f.portals.flamePosition(party));
    party[0].position.y = 0;
    f.portals.consume(0);
    CHECK_FALSE(f.portals.flamePosition(party));
}

TEST_CASE("exit portal artwork retains authored yaw pitch and roll", "[portals][item-rotation]") {
    const auto dir = sampleLevel("portals-rotation");
    writeTextFile(dir / "world.json", R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
        "itemInfos":[{"type":9,"name":"EXIT_PORTAL"}],"itemInstances":[
        {"info":0,"position":[10,0,10],"rotation":[0,1.570796327,0]},
        {"info":0,"position":[20,0,10],"rotation":[1.570796327,0,0]},
        {"info":0,"position":[30,0,10],"rotation":[3.141592654,0,3.141592654]}]})");
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 0 0 1\nv 1 0 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
        {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
        {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"EXIT_PORTAL",
        "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
        "sequences":[{"name":"IDLE","frames":10,"rate":30}]}]})");
    test::FakeRenderDevice device;
    WorldLayout layout;
    const LevelCatalog catalog;
    ItemArchive art;
    ExitPortals portals;
    test::convertModelFixture(dir);
    REQUIRE(layout.load(dir));
    REQUIRE(art.load(dir));
    REQUIRE(portals.bind(device, layout, art, catalog, nullptr));
    portals.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 3);
    const std::array forward{Vec3{-1, 0, 0}, Vec3{0, -1, 0}, Vec3{0, 0, -1}};
    const std::array right{Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{-1, 0, 0}};
    for (usize i = 0; i < forward.size(); ++i) {
        CAPTURE(i);
        const auto& draw = device.draws[i];
        REQUIRE(draw.vertices.size() == 3);
        CHECK(glm::distance(draw.vertices[0].position, Vec3{10 + 10 * i, 0, 10}) < 0.0001f);
        CHECK(glm::distance(draw.vertices[1].position - draw.vertices[0].position, forward[i]) <
              0.0001f);
        CHECK(glm::distance(draw.vertices[2].position - draw.vertices[0].position, right[i]) <
              0.0001f);
    }
    // These synthetic portals share an empty tag. A fully hidden reveal submits
    // no geometry; restoring alpha retains all three authored transforms.
    portals.setAlpha("", 0);
    device.draws.clear();
    portals.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.empty());
    portals.setAlpha("", 1);
    portals.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() == 3);
    portals.clear();
}

TEST_CASE("a portal runs through with the whole party on it and waits for stragglers",
          "[game][world][portals]") {
    Fixture f("portals-run");
    const PortalVisitor on{Vec3{11.0f, 0.2f, 10.0f}, 0.75f};
    const PortalVisitor off{Vec3{30.0f, 0.0f, 10.0f}, 0.75f};
    // Nobody near: it idles.
    REQUIRE_FALSE(f.run(std::array<PortalVisitor, 1>{off}, 60).has_value());
    REQUIRE(f.portals.portal(0).action == 0);
    // One of two on it: it wakes as far as the waiting sequence and holds there.
    const std::array<PortalVisitor, 2> split{on, off};
    REQUIRE_FALSE(f.run(split, 200).has_value());
    REQUIRE(f.portals.portal(0).action == ExitPortals::kWaiting);
    // Moving on it, nobody is told to wait; standing still, the one on it is.
    CHECK(f.portals.takeWaiting().empty());
    PortalVisitor waiting = on;
    waiting.party = 1;
    waiting.still = true;
    f.run(std::array<PortalVisitor, 2>{waiting, off}, 1);
    CHECK(f.portals.takeWaiting() == std::vector<s32>{1});
    f.run(std::array<PortalVisitor, 1>{waiting}, 1); // alone, there is nobody to wait for
    CHECK(f.portals.takeWaiting().empty());
    // The other arrives: transport starts with the raised glow still held.
    const std::array<PortalVisitor, 2> together{on, PortalVisitor{Vec3{9.0f, 0.0f, 11.0f}, 0.75f}};
    const auto left = f.run(together, 200);
    REQUIRE(left == 0U);
    REQUIRE(f.portals.portal(0).action == ExitPortals::kWaiting);
    // The far portal never stirred.
    REQUIRE(f.portals.portal(1).action == 0);
}

TEST_CASE("a portal left alone plays itself out and goes back to idle", "[game][world][portals]") {
    Fixture f("portals-alone");
    const std::array<PortalVisitor, 1> on{PortalVisitor{Vec3{10.0f, 0.0f, 10.0f}, 0.75f}};
    f.portals.update(2, 1.0f / 30.0f, on);
    REQUIRE(f.portals.portal(0).action == 1); // it wakes the moment it is stepped on
    const std::array<PortalVisitor, 1> away{PortalVisitor{Vec3{40.0f, 0.0f, 10.0f}, 0.75f}};
    REQUIRE_FALSE(f.run(away, 400).has_value());
    REQUIRE(f.portals.portal(0).action == 0);
    // Too far over it does not count as standing on it; a larger party has a wider portal.
    const std::array<PortalVisitor, 1> above{PortalVisitor{Vec3{10.0f, 9.0f, 10.0f}, 0.75f}};
    REQUIRE_FALSE(f.run(above, 30).has_value());
    REQUIRE(f.portals.portal(0).action == 0);
    const PortalVisitor edge{Vec3{14.2f, 0.0f, 10.0f}, 0.75f}; // 4.2 off: past 3 + 0.75
    REQUIRE_FALSE(f.run(std::array<PortalVisitor, 1>{edge}, 10).has_value());
    REQUIRE(f.portals.portal(0).action == 0);
    const std::array<PortalVisitor, 2> pair{edge, PortalVisitor{Vec3{10.0f, 0.0f, 10.0f}, 0.75f}};
    f.portals.update(2, 1.0f / 30.0f, pair);
    REQUIRE(f.portals.portal(0).action == 1);
}

TEST_CASE("the real portal holds ACTIVE2 without replaying ACTIVE1 for a waiting party",
          "[portals][assets]") {
    Fixture f("portals-real-loop");
    const auto archive = test::assetOrSkip("ITEMS/LEVELL/ANIM.PS2").parent_path();
    REQUIRE(f.items.load(archive));
    REQUIRE(f.portals.bind(f.device, f.layout, f.items, f.catalog, nullptr));
    const std::array split{PortalVisitor{Vec3{10, 0, 10}}, PortalVisitor{Vec3{30, 0, 10}}};
    REQUIRE_FALSE(f.run(split, 200));
    REQUIRE(f.portals.portal(0).action == ExitPortals::kWaiting);
    const auto sequence = f.portals.portal(0).player.sequence();
    f32 previous = f.portals.portal(0).player.frame();
    bool wrapped = false;
    for (s32 i = 0; i < 120; ++i) {
        REQUIRE_FALSE(f.portals.update(1, 1.0f / 60, split));
        const auto& portal = f.portals.portal(0);
        CHECK(portal.action == ExitPortals::kWaiting);
        CHECK(portal.player.sequence() == sequence);
        CHECK(portal.ticksLeft == ExitPortals::kWaitingTicks - 1);
        wrapped |= portal.player.frame() < previous;
        previous = portal.player.frame();
    }
    CHECK(wrapped);
    const std::array together{split[0], split[0]};
    // The wait does not expire before the missing player arrives.
    for (s32 i = 0; i < ExitPortals::kWaitingTicks - 1; ++i) {
        f.portals.update(1, 1.0f / 60, together);
        CHECK(f.portals.portal(0).action == ExitPortals::kWaiting);
    }
    REQUIRE(f.run(together, 120) == 0);
    const auto finalSequence = f.portals.portal(0).player.sequence();
    // Rendering must retain the tall column through many wraps, not merely
    // keep the same action id while showing a startup mesh again.
    for (s32 frame = 0; frame < 300; ++frame) {
        f.portals.animate(1.0f / 60);
        f.device.draws.clear();
        f.portals.draw(f.device, Mat4{1}, {});
        f32 top = 0;
        for (const auto& draw : f.device.draws) {
            top = std::max(top, test::maxCorner(draw).y);
        }
        CHECK(top > 12);
        CHECK(f.portals.portal(0).player.sequence() == finalSequence);
    }
    CHECK(f.portals.portal(0).action == ExitPortals::kWaiting);
    CHECK(f.portals.portal(0).player.sequence() == finalSequence);
    // Only abandoning the platform closes the column.
    const std::array away{split[1], split[1]};
    REQUIRE_FALSE(f.portals.update(1, 1.0f / 60, away));
    CHECK(f.portals.portal(0).action == ExitPortals::kLast);
    // Rejoining during the closing sequence must start a fresh visit, not
    // repeatedly restart ACTIVE3 or leave the portal unable to transport.
    REQUIRE(f.run(together, 180) == 0);
    CHECK(f.portals.portal(0).action == ExitPortals::kWaiting);

    // ACTIVE3's stored meshes grow, but flag 1 plays them in reverse so the
    // closing flame shrinks rather than showing the startup a second time.
    const auto treeIndex = f.items.trees.find(ExitPortals::kFigure);
    REQUIRE(treeIndex);
    const auto& tree = f.items.trees.tree(*treeIndex);
    const auto closing = tree.findSequence("ACTIVE3");
    REQUIRE(closing);
    CHECK(tree.sequences[*closing].effectFrame(0) == 14);
    TreeModel model;
    REQUIRE(model.bind(tree, f.items.models, f.items.textures, f.device));
    const auto height = [&](s32 frame) {
        f.device.draws.clear();
        model.setFrame(*closing, frame);
        model.draw(f.device, Mat4{1}, Mat4{1});
        f32 top = 0;
        for (const auto& draw : f.device.draws) {
            top = std::max(top, test::maxCorner(draw).y);
        }
        return top;
    };
    CHECK(height(0) > height(14) + 5);
}

TEST_CASE("a secret icon takes one toucher, keeps its authored radius, and is consumed once",
          "[secret][portals]") {
    const auto dir = sampleLevel("portals-secret");
    writeTextFile(dir / "wdata/SECRET.json", R"({"realm":12,"prefix":"levelS",
        "levels":[{"name":"S1","title":"Secret"}]})");
    writeTextFile(
        dir / "world.json",
        R"({"objects":[{"name":"GROUND","position":[0,0,0],"next":-1,"child":-1}],"locators":[],
        "itemInfos":[{"type":9,"subtype":50,"name":"SECRET_ICON","radius":1}],
        "itemInstances":[{"info":0,"minPlayers":1,"position":[10,0,10],
        "rotation":[0,0,0],"params":[0,0,0,0,83,49,0,0,0,0,0,0]}]})");
    test::FakeRenderDevice device;
    WorldLayout layout;
    LevelCatalog catalog;
    ItemArchive items;
    ExitPortals portals;
    REQUIRE(layout.load(dir));
    REQUIRE(catalog.load(dir));
    REQUIRE(portals.bind(device, layout, items, catalog, nullptr));
    REQUIRE(portals.size() == 1);
    REQUIRE(portals.portal(0).secret);
    REQUIRE(portals.portal(0).destination);
    CHECK(portals.portal(0).destination->isSecret());
    const std::array near{PortalVisitor{Vec3{12, 0, 10}, 0.5f},
                          PortalVisitor{Vec3{30, 0, 30}, 0.5f}};
    CHECK_FALSE(portals.update(2, 1.0f / 30, near)); // no party-size radius bonus
    const std::array touching{PortalVisitor{Vec3{11, 0, 10}, 0.5f}, near[1]};
    CHECK(portals.update(2, 1.0f / 30, touching) == 0);
    CHECK(portals.portal(0).departurePosition == touching[0].position);
    portals.consume(0);
    CHECK_FALSE(portals.update(2, 1.0f / 30, touching));
}

TEST_CASE("a portal the tower keeps shut never wakes and takes nobody",
          "[game][world][portals][tower-access]") {
    Fixture f("portals-shut");
    // The sample's "g1" is the town's first portal, which stands whatever has been beaten.
    std::vector<ClassProgress> party(1);
    TowerAccess access{party};
    REQUIRE(f.portals.bind(f.device, f.layout, f.items, f.catalog, nullptr, nullptr, &access));
    REQUIRE_FALSE(f.portals.portal(0).shut); // a realm's first portal always stands
    REQUIRE(f.portals.shutGates().empty());
    REQUIRE(ExitPortals::gateOf("g1") == 0);
    REQUIRE(ExitPortals::gateOf("b6") == 5);
    REQUIRE(ExitPortals::gateOf("g") == -1);
    // A second level's portal with the first not beaten: shut, and inert to the whole party.
    const auto dir = sampleLevel("portals-shut-second");
    writeTextFile(dir / "wdata/TOWN.json", R"({"realm": 7, "prefix": "levelG", "levels": [
      {"name": "G1", "title": "Fields"}, {"name": "G2", "title": "Farm"}]})");
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "GROUND", "position": [0, 0, 0], "next": -1, "child": -1}],
  "itemInfos": [{"type": 9, "subtype": 0, "name": "EXIT_PORTAL", "radius": 3, "height": 2}],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [10, 0, 10], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 103, 50, 0, 0, 0, 0, 0, 0]}]})");
    REQUIRE(f.layout.load(dir));
    REQUIRE(f.catalog.load(dir));
    REQUIRE(f.portals.bind(f.device, f.layout, f.items, f.catalog, nullptr, nullptr, &access));
    REQUIRE(f.portals.portal(0).tag == "g2");
    REQUIRE(f.portals.portal(0).shut);
    REQUIRE(f.portals.shutGates() == std::vector{ExitPortals::ShutGate{7, 1}});
    const std::array on{PortalVisitor{Vec3{10, 0, 10}, 0.75f}};
    REQUIRE_FALSE(f.run(on, 400).has_value());
    REQUIRE(f.portals.portal(0).action == 0);
    REQUIRE(f.portals.takeWaiting().empty());
    // With the first beaten it opens like any other.
    party[0].levels.recordBeaten(7, 0);
    access = TowerAccess{party};
    REQUIRE(f.portals.bind(f.device, f.layout, f.items, f.catalog, nullptr, nullptr, &access));
    REQUIRE_FALSE(f.portals.portal(0).shut);
    REQUIRE(f.run(on, 400) == 0);
}

TEST_CASE("the real tower shuts the portals past what the party has beaten, wearing EXIT_OFF",
          "[game][world][portals][tower-access][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const auto archive = test::assetOrSkip("ITEMS/LEVELL/ANIM.PS2").parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    LevelCatalog catalog;
    ItemArchive items;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    REQUIRE(catalog.load(root));
    REQUIRE(items.load(archive));
    REQUIRE(items.models.find(TowerAccess::kOffFigure).has_value());
    std::vector<ClassProgress> party(1);
    party[0].levels.recordBeaten(7, 0); // the town's first: its second opens, its third not
    const TowerAccess access{party};
    ExitPortals portals;
    REQUIRE(portals.bind(device, layout, items, catalog, nullptr, nullptr, &access));
    const auto named = [&](std::string_view tag) -> const ExitPortals::Portal& {
        for (usize i = 0; i < portals.size(); ++i) {
            if (portals.portal(i).tag == tag) {
                return portals.portal(i);
            }
        }
        FAIL("no portal " << tag);
        return portals.portal(0);
    };
    CHECK_FALSE(named("g1").shut);
    CHECK_FALSE(named("g2").shut);
    CHECK(named("g3").shut);
    CHECK(named("g3").model.bound());
    CHECK_FALSE(named("b1").shut);
    CHECK(named("b2").shut);
    CHECK(named("e1").shut); // the temple, the underworld and the battlefield want the shards
    CHECK(named("f1").shut);
    CHECK(named("h1").shut);
    const auto gates = portals.shutGates();
    CHECK(std::ranges::find(gates, ExitPortals::ShutGate{7, 2}) != gates.end());
    CHECK(std::ranges::find(gates, ExitPortals::ShutGate{5, 0}) != gates.end());
    CHECK(std::ranges::find(gates, ExitPortals::ShutGate{7, 1}) == gates.end());
    CHECK(gates.size() == portals.size() - 9); // the first portals of the eight crystal realms, g2
    // The shut one draws the lone EXIT_OFF mesh where it stands, unposed.
    device.draws.clear();
    portals.draw(device, Mat4{1}, {});
    const Vec3 spot = named("g3").position;
    bool drawn = false;
    for (const auto& draw : device.draws) {
        for (const auto& vertex : draw.vertices) {
            drawn = drawn || glm::distance(vertex.position, spot) < 6.0f;
        }
    }
    CHECK(drawn);
    // Standing on it does nothing; the open one beside it wakes.
    const std::array on{PortalVisitor{named("g3").position, 0.75f}};
    for (s32 i = 0; i < 100; ++i) {
        CHECK_FALSE(portals.update(2, 1.0f / 30, on).has_value());
    }
    CHECK(named("g3").action == 0);
    const std::array beside{PortalVisitor{named("g2").position, 0.75f}};
    portals.update(2, 1.0f / 30, beside);
    CHECK(named("g2").action == 1);
    portals.clear();
}

TEST_CASE("portal solid platforms and depthless columns draw in separate passes",
          "[portals][visual-parity]") {
    const auto dir = sampleLevel("portal-passes");
    writeTextFile(dir / "body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
    writeFile(dir / "skin.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"EXIT_PORTAL",
      "nodes":[{"name":"BASE","object":"BODY","parent":-1,"position":[0,0,0]},
        {"name":"COLUMN","object":"BODY","parent":0,"position":[0,0,0],"objectFlags":128}],
      "sequences":[{"name":"IDLE","frames":10}]}]})");
    test::FakeRenderDevice device;
    WorldLayout layout;
    const LevelCatalog catalog;
    ItemArchive art;
    ExitPortals portals;
    test::convertModelFixture(dir);
    REQUIRE(layout.load(dir));
    REQUIRE(art.load(dir));
    REQUIRE(portals.bind(device, layout, art, catalog, nullptr));
    for (const auto pass : {TreeModel::Pass::DepthWriting, TreeModel::Pass::Effects}) {
        device.draws.clear();
        portals.draw(device, Mat4{1}, {}, nullptr, pass);
        REQUIRE(device.draws.size() == 2);
        for (const auto& draw : device.draws) {
            CHECK(draw.state.depthWrite == (pass == TreeModel::Pass::DepthWriting));
            CHECK(draw.state.depthTest);
        }
    }
}

} // namespace
