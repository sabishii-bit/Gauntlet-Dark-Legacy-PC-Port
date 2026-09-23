#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/ExitPortals.h"

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
    f.portals.clear();
    REQUIRE(f.portals.size() == 0);
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
    // The other arrives: the last sequence plays and the party is through.
    const std::array<PortalVisitor, 2> together{on, PortalVisitor{Vec3{9.0f, 0.0f, 11.0f}, 0.75f}};
    const auto left = f.run(together, 200);
    REQUIRE(left == 0U);
    REQUIRE(f.portals.portal(0).action == ExitPortals::kLast);
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

} // namespace
