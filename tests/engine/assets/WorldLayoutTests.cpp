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
    {"name": "TORCH", "position": [2, 2, 2], "next": -1, "child": -1, "radius": 1,
     "flags": 2048, "objectFlags": 8390784}
  ],
  "animations": [
    {"object": 1, "frames": 4, "state": 257, "start": 0,
     "track": {"flags": 4, "frames": [0, 3], "values": [0, 1.5]}}
  ],
  "particles": [
    {"id": "E", "preset": 5, "flags": 648, "flagMask": 648, "enables": 355297,
     "particleLife": [0.2, 0.22], "angle": 80, "texture": "p_torch",
     "direction": [0, 1, 0], "volume": [0.1, 0.3, 0.1], "rate": [25, 25, 25, 25],
     "gravity": -0.38, "speed": 4, "rgba": [0, 16777215, 16777215, 0],
     "width": [2.8, 2, 2, 0.1]}
  ],
  "itemInfos": [
    {"type": 1, "subtype": 15, "name": "gemorange", "radius": 0.1, "height": 2,
     "value": 4, "armor": -1, "activeType": 16},
    {"type": 2, "subtype": 46, "name": "CHEST"}
  ],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "flags": 0, "name": "", "position": [19.3, -2, -57.5],
     "rotation": [0, 1.5, 0], "params": [7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 1, "minPlayers": 12, "name": "chestg3", "position": [1, 2, 3]}
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

TEST_CASE("a layout carries its animations and tells its objects' roles", "[assets][world]") {
    WorldLayout layout;
    REQUIRE(layout.load(sampleLayout("world-layout-animations")));
    REQUIRE(layout.animations().size() == 1);
    const WorldAnimation& turn = layout.animations()[0];
    REQUIRE(turn.object == 1);
    REQUIRE(turn.frames == 4);
    REQUIRE(turn.state == 0x101);
    REQUIRE(turn.track.has(2));
    REQUIRE(turn.track.frames == std::vector<u16>{0, 3});
    REQUIRE(turn.track.values[1] == 1.5f);
    const WorldObject& torch = layout.objects()[3];
    REQUIRE(torch.particles());
    REQUIRE(torch.sorted());
    REQUIRE(torch.additive());
    REQUIRE_FALSE(layout.objects()[1].particles());
    // The particle template, found by its letter.
    REQUIRE(layout.particleTemplates().size() == 1);
    const ParticleTemplate* flame = layout.findParticleTemplate('E');
    REQUIRE(flame != nullptr);
    REQUIRE(layout.findParticleTemplate('Z') == nullptr);
    REQUIRE(flame->preset == 5);
    REQUIRE(flame->sets(ParticleTemplate::kPreset));
    REQUIRE(flame->sets(ParticleTemplate::kColor));
    REQUIRE_FALSE(flame->sets(ParticleTemplate::kAlpha));
    REQUIRE(flame->decides(ParticleTemplate::kAdditive));
    REQUIRE(flame->flag(ParticleTemplate::kAdditive));
    REQUIRE_FALSE(flame->decides(ParticleTemplate::kForever));
    REQUIRE(flame->texture == "P_TORCH");
    REQUIRE(flame->particleLife[1] == 0.22f);
    REQUIRE(flame->rgba[1] == 0x00FFFFFF);
    REQUIRE(flame->width[3] == 0.1f);
    // The items it places.
    REQUIRE(layout.itemInfos().size() == 2);
    REQUIRE(layout.itemInfos()[0].type == ItemInfo::kPowerup);
    REQUIRE(layout.itemInfos()[0].subtype == ItemInfo::kCrystal);
    REQUIRE(layout.itemInfos()[0].name == "GEMORANGE");
    REQUIRE(layout.itemInfos()[0].armor == -1);
    REQUIRE(layout.itemInfos()[1].type == ItemInfo::kContainer);
    REQUIRE(layout.itemInstances().size() == 2);
    REQUIRE(layout.itemInstances()[0].info == 0);
    REQUIRE(layout.itemInstances()[0].minPlayers == 1);
    REQUIRE(layout.itemInstances()[0].position == Vec3{19.3f, -2.0f, -57.5f});
    REQUIRE(layout.itemInstances()[0].rotation.y == 1.5f);
    REQUIRE(layout.itemInstances()[0].params[0] == 7);
    REQUIRE(layout.itemInstances()[1].name == "CHESTG3");
    REQUIRE(layout.itemInstances()[1].minPlayers == 12);
    // An animation naming a missing object or with broken keys fails the load.
    const auto dir = test::scratchDirectory("world-layout-bad-animation");
    writeTextFile(dir / "world.json", R"({"objects": [{"name": "A", "position": [0, 0, 0]}],
      "animations": [{"object": 4, "frames": 2, "track": {"flags": 1, "frames": [0], "values": [1]}}]})");
    REQUIRE_FALSE(layout.load(dir));
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
