#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

std::filesystem::path sampleLayout(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "world.json", R"({
  "bounds": {"min": [-10, -1, -10], "max": [10, 5, 10]},
  "objects": [
    {"name": "ROOT", "position": [1, 2, 3], "flags": 4, "objectFlags": 0, "next": -1, "child": 1, "radius": 0},
    {"name": "WALL", "position": [0.5, 0, 0], "flags": 6, "objectFlags": 32768, "next": 2, "child": -1, "radius": 7},
    {"name": "FLOOR", "position": [0, -1, 0], "flags": 6, "next": -1, "child": 3, "radius": 9},
    {"name": "TORCH", "position": [2, 2, 2], "next": -1, "child": -1, "radius": 1}
  ],
  "locators": [
    {"type": "cameraGame", "delay": 0, "next": 0, "position": [0, 4, 17], "rotation": [0.5, 3.1, 0]},
    {"type": "cameraStart", "delay": 0, "next": 1, "position": [1, 1, 1], "rotation": [0, 0, 0]},
    {"type": "cameraStart", "delay": 2, "next": 0, "position": [3, 20, -9], "rotation": [0.55, 3.14, 0]},
    {"type": "mystery", "delay": 0, "next": 0, "position": [0, 0, 0], "rotation": [0, 0, 0]}
  ]
})");
    return dir;
}

TEST_CASE("locator kinds have manifest names", "[assets][world]") {
    REQUIRE(locatorKindName(LocatorKind::CameraStart) == "cameraStart");
    REQUIRE(locatorKindName(LocatorKind::Event) == "event");
    REQUIRE(locatorKindName(LocatorKind::Count).empty());
    REQUIRE(locatorKindFromName("start") == LocatorKind::Start);
    REQUIRE(locatorKindFromName("triggerCamera") == LocatorKind::TriggerCamera);
    REQUIRE_FALSE(locatorKindFromName("Start").has_value());
}

TEST_CASE("a layout resolves parents and sums positions down the tree", "[assets][world]") {
    WorldLayout layout;
    REQUIRE(layout.load(sampleLayout("world-layout")));
    REQUIRE(layout.loaded());
    REQUIRE(layout.objects().size() == 4);
    REQUIRE(layout.minBounds() == Vec3{-10.0f, -1.0f, -10.0f});
    REQUIRE(layout.maxBounds().y == 5.0f);
    REQUIRE(layout.objects()[0].parent == -1);
    REQUIRE(layout.objects()[1].parent == 0);
    REQUIRE(layout.objects()[2].parent == 0);
    REQUIRE(layout.objects()[3].parent == 2);
    REQUIRE(layout.objects()[1].objectFlags == WorldObject::kChrome);
    REQUIRE(layout.worldPosition(0) == Vec3{1.0f, 2.0f, 3.0f});
    REQUIRE(layout.worldPosition(1) == Vec3{1.5f, 2.0f, 3.0f});
    REQUIRE(layout.worldPosition(3) == Vec3{3.0f, 3.0f, 5.0f});
}

TEST_CASE("locators are found by kind and index", "[assets][world]") {
    WorldLayout layout;
    REQUIRE(layout.load(sampleLayout("world-layout-locators")));
    REQUIRE(layout.locators().size() == 4);
    const WorldLocator* start = layout.findLocator(LocatorKind::CameraStart);
    REQUIRE(start != nullptr);
    REQUIRE(start->position == Vec3{3.0f, 20.0f, -9.0f});
    REQUIRE(start->delay == 2);
    REQUIRE(layout.findLocator(LocatorKind::CameraStart, 1)->position.x == 1.0f);
    REQUIRE(layout.findLocator(LocatorKind::CameraGame)->rotation.y == 3.1f);
    REQUIRE(layout.findLocator(LocatorKind::Boss) == nullptr);
    REQUIRE(layout.locators()[3].kind == LocatorKind::None);
}

TEST_CASE("a missing or malformed layout fails to load", "[assets][world]") {
    WorldLayout layout;
    REQUIRE_FALSE(layout.load(test::scratchDirectory("world-layout-missing")));
    REQUIRE_FALSE(layout.loaded());
    const auto dir = test::scratchDirectory("world-layout-bad");
    writeTextFile(dir / "world.json", R"({"objects": [{"name": 3}]})");
    REQUIRE_FALSE(layout.load(dir));
    REQUIRE(layout.objects().empty());
}

TEST_CASE("the unpacked tower layout places its objects and cameras", "[assets][world][unpacked]") {
    const std::filesystem::path dir =
        test::unpackedOrSkip("LEVELS/LEVELL1/world.json").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    REQUIRE(layout.objects().size() > 3000);
    REQUIRE(layout.findLocator(LocatorKind::CameraStart) != nullptr);
    REQUIRE(layout.findLocator(LocatorKind::Start) != nullptr);
}

} // namespace
