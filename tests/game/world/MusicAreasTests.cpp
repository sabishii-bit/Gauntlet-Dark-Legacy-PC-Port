#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/AmbientSounds.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelWorld.h"
#include "game/world/MusicAreas.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** A level with two music zones, the second's area higher and overlapping the first's, and a
 * plain ambient loop: the zones are sound items whose second parameter word names an area. */
std::filesystem::path writeZonedLevel(std::string_view name) {
    const auto level = test::scratchDirectory(std::string(name) + "-level");
    // The radius leads the parameters as a float (20 and 10), the area follows as a word,
    // then the way over: the first zone fades, the second cuts over at once.
    writeTextFile(level / "world.json", R"({
  "objects": [
    {"name": "FLOOR", "position": [0, 0, 0], "next": -1, "child": -1}
  ],
  "locators": [],
  "itemInfos": [
    {"type": 13, "subtype": 0, "name": ""}
  ],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "name": "", "position": [0, 0, 0],
     "rotation": [0, 0, 0], "params": [0, 0, 160, 65, 1, 0, 0, 0, 1, 0, 0, 0]},
    {"info": 0, "minPlayers": 1, "name": "", "position": [15, 0, 0],
     "rotation": [0, 0, 0], "params": [0, 0, 32, 65, 2, 0, 0, 0, 2, 0, 0, 0]},
    {"info": 0, "minPlayers": 1, "name": "S_SFIREL", "position": [40, 0, 0],
     "rotation": [0, 0, 0], "params": [0, 0, 128, 64, 0, 0, 0, 0, 0, 0, 0, 0]}
  ]
})");
    return level;
}

TEST_CASE("music zones bind by area and the ambience leaves them alone",
          "[game][world][music-areas]") {
    WorldLayout layout;
    REQUIRE(layout.load(writeZonedLevel("music-zones-bind")));
    MusicAreas areas;
    REQUIRE(areas.bind(layout));
    REQUIRE(areas.size() == 2);
    REQUIRE(areas.zone(0).instance == 0);
    REQUIRE(areas.zone(0).radius == 20.0f);
    REQUIRE(areas.zone(0).area == 0);
    REQUIRE(areas.zone(0).how == MusicSwitch::Faded);
    REQUIRE(areas.zone(1).instance == 1);
    REQUIRE(areas.zone(1).radius == 10.0f);
    REQUIRE(areas.zone(1).area == 1);
    REQUIRE(areas.zone(1).how == MusicSwitch::AtOnce);
    // The way over: nought (and under) waits for the part's end, past one cuts over.
    ItemInstance instance;
    REQUIRE(MusicAreas::switchOf(instance) == MusicSwitch::AtPartEnd);
    instance.params[8] = 0xFF;
    instance.params[9] = 0xFF;
    REQUIRE(MusicAreas::switchOf(instance) == MusicSwitch::AtPartEnd);
    instance.params[8] = 5;
    instance.params[9] = 0;
    REQUIRE(MusicAreas::switchOf(instance) == MusicSwitch::AtOnce);
    REQUIRE(MusicAreas::areaOf(instance) == 0);
    // The zones are no ambient loops: nothing binds for them, without a warning.
    AmbientSounds ambience;
    SoundSet none;
    const std::array<SoundSet*, 1> banks{&none};
    REQUIRE_FALSE(ambience.bind(layout, banks));
    areas.clear();
    REQUIRE(areas.size() == 0);
}

TEST_CASE("a crossing asks for the area once and the highest area wins where zones overlap",
          "[game][world][music-areas]") {
    WorldLayout layout;
    REQUIRE(layout.load(writeZonedLevel("music-zones-cross")));
    MusicAreas areas;
    REQUIRE(areas.bind(layout));
    s32 current = 0; // the first area plays from the start
    const auto listen = [&](f32 x) {
        const std::array<Vec3, 1> party{Vec3{x, 0.0f, 0.0f}};
        return areas.update(party, current);
    };
    // Outside every zone nothing is asked for; inside the first, the area playing already.
    REQUIRE_FALSE(listen(-30.0f).has_value());
    REQUIRE_FALSE(listen(-10.0f).has_value());
    REQUIRE_FALSE(areas.pick(std::array<Vec3, 0>{}).has_value());
    // Into the second zone, where both hold the listener: the higher area, its own way over.
    std::optional<MusicCue> cue = listen(12.0f);
    REQUIRE(cue.has_value());
    REQUIRE(cue->area == 1);
    REQUIRE(cue->how == MusicSwitch::AtOnce);
    current = cue->area;
    // Deeper inside, nothing more; out of both zones again, nothing either.
    REQUIRE_FALSE(listen(18.0f).has_value());
    REQUIRE_FALSE(listen(30.0f).has_value());
    // Back into the first zone alone: its area, with a fade. A zone's edge is outside it.
    cue = listen(-19.0f);
    REQUIRE(cue.has_value());
    REQUIRE(cue->area == 0);
    REQUIRE(cue->how == MusicSwitch::Faded);
    current = cue->area;
    REQUIRE_FALSE(listen(-20.0f).has_value());
    REQUIRE_FALSE(listen(-19.0f).has_value());
    // The nearest of a party decides: one member inside the second zone is enough.
    const std::array<Vec3, 2> party{Vec3{-100.0f, 0.0f, 0.0f}, Vec3{20.0f, 5.0f, 0.0f}};
    cue = areas.update(party, current);
    REQUIRE(cue.has_value());
    REQUIRE(cue->area == 1);
}

TEST_CASE("the shipped levels' music zones name their realm's stream areas",
          "[game][world][music-areas][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELJ3/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("audio/TOWAMB.vbk");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELJ3"));
    MusicAreas areas;
    REQUIRE(areas.bind(layout));
    // Three zones of twenty units for dream3's three areas: the first cuts over, the other
    // two wait for the part playing to end.
    REQUIRE(areas.size() == 3);
    std::array<usize, 3> counted{};
    for (usize i = 0; i < areas.size(); ++i) {
        const MusicZone& zone = areas.zone(i);
        REQUIRE(zone.radius == 20.0f);
        REQUIRE(zone.area >= 0);
        REQUIRE(zone.area < 3);
        ++counted[static_cast<usize>(zone.area)];
        REQUIRE(zone.how == (zone.area == 0 ? MusicSwitch::AtOnce : MusicSwitch::AtPartEnd));
    }
    REQUIRE(counted == std::array<usize, 3>{1, 1, 1});
    // Standing at a zone's centre asks for its area.
    const std::array<Vec3, 1> party{areas.zone(0).position};
    const std::optional<MusicCue> cue = areas.update(party, 0);
    REQUIRE(cue.has_value());
    REQUIRE(cue->area == areas.zone(0).area);
    // The zones are not the level's ambience.
    SoundSet ambient;
    REQUIRE(ambient.load(root / "audio/TOWAMB"));
    AmbientSounds ambience;
    const std::array<SoundSet*, 1> banks{&ambient};
    ambience.bind(layout, banks);
    for (usize i = 0; i < ambience.size(); ++i) {
        REQUIRE(MusicAreas::areaOf(
                    layout.itemInstances()[static_cast<usize>(ambience.emitter(i).instance)]) == 0);
    }
}

TEST_CASE("music zones honor minimum and exact joined populations independently of listeners",
          "[game][world][music-areas][ambient-population]") {
    const auto directory = test::scratchDirectory("music-zone-population");
    writeTextFile(directory / "world.json", R"({
      "objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[{"type":13}], "itemInstances":[
        {"info":0,"minPlayers":3,"position":[0,0,0],
         "params":[0,0,32,65,2,0,0,0,2,0,0,0]},
        {"info":0,"minPlayers":12,"position":[0,0,0],
         "params":[0,0,32,65,3,0,0,0,1,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    MusicAreas areas;
    REQUIRE(areas.bind(layout));
    const std::array listener{Vec3{0}};
    for (const s32 joined : {1, 2, 3, 4, 1}) {
        CAPTURE(joined);
        areas.setPlayerCount(joined);
        const auto cue = areas.pick(listener);
        REQUIRE(cue.has_value() == (joined > 1));
        if (cue) {
            CHECK(cue->area == (joined == 2 ? 2 : 1));
            CHECK(cue->how == (joined == 2 ? MusicSwitch::Faded : MusicSwitch::AtOnce));
        }
        CHECK_FALSE(areas.pick({}).has_value());
    }
    areas.setPlayerCount(3);
    REQUIRE(areas.bind(layout));
    CHECK_FALSE(areas.pick(listener).has_value());
}

TEST_CASE("music zones bind the initially posed node and retain its identity as it moves",
          "[game][world][music-areas][music-platform]") {
    const auto directory = test::scratchDirectory("music-moving-parent");
    writeTextFile(directory / "world.json", R"({
      "objects":[{"name":"LIFT","position":[100,0,0],"flags":4096}],
      "animations":[{"object":0,"frames":2,"state":257,
        "track":{"flags":16,"frames":[0,1],"values":[0,100]}}],
      "itemInfos":[{"type":13,"name":""}],
      "itemInstances":[
        {"info":0,"position":[0,0,0],"params":[0,0,0,64,2,0,0,0,2,0,0,0]},
        {"info":0,"position":[50,0,0],"params":[0,0,0,64,1,0,0,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldScene scene;
    // A placement-only scene needs no mesh, but keeps the same node transforms.
    scene.build(layout, models, textures, device);
    REQUIRE(scene.moving(0));
    scene.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{4, 0, 0}));
    MusicAreas areas;
    REQUIRE(areas.bind(layout, &scene));
    REQUIRE(areas.zone(0).parent == 0);
    REQUIRE(areas.zone(1).parent == -1);
    const auto at = [&](const Vec3& position) {
        return areas.update(std::array{position}, MusicAreas::kNoArea, &scene);
    };
    REQUIRE(at(Vec3{4, 0, 0}).has_value());
    CHECK(at(Vec3{4, 0, 0})->area == 1);
    // The node's center replaces the authored center; its four-unit offset is not kept.
    CHECK_FALSE(at(Vec3{0}).has_value());
    scene.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{104, 30, -20}));
    CHECK_FALSE(at(Vec3{4, 0, 0}).has_value());
    REQUIRE(at(Vec3{104, 30, -20}).has_value());
    CHECK(at(Vec3{104, 30, -20})->how == MusicSwitch::AtOnce);
    CHECK_FALSE(at(Vec3{104, 32, -20}).has_value());
    REQUIRE(at(Vec3{50, 0, 0}).has_value());
    CHECK(at(Vec3{50, 0, 0})->area == 0);
    areas.clear();
    CHECK(areas.size() == 0);
}

TEST_CASE("the Dream World's elevator carries its native music-area center",
          "[game][world][music-areas][music-platform][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELJ6/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("J6");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    world.setPlayerCount(1);
    world.startTriggers({});
    const WorldScene& scene = world.scene();
    MusicAreas areas;
    REQUIRE(areas.bind(world.layout(), &scene));
    const MusicZone* elevator = nullptr;
    for (usize i = 0; i < areas.size(); ++i) {
        if (areas.zone(i).instance == 626) {
            elevator = &areas.zone(i);
        }
    }
    REQUIRE(elevator != nullptr);
    REQUIRE(elevator->parent == 686);
    const LevelTrigger* pad = nullptr;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        if (world.triggers().trigger(i).instance == 22) {
            pad = &world.triggers().trigger(i);
        }
    }
    REQUIRE(pad != nullptr);
    REQUIRE(pad->target == 686);
    REQUIRE_FALSE(pad->chained);
    // Exercise the native BRIDGEPAD through a real standing visitor, not a forced
    // activation or an arbitrary scene transform. The visitor rides the same floor.
    TriggerVisitor visitor;
    visitor.position = pad->spot;
    visitor.floorObject = 688;
    visitor.party = 0;
    const Vec3 start{scene.worldTransform(686)[3]};
    const Vec3 local = Vec3{glm::inverse(scene.worldTransform(688)) * Vec4{visitor.position, 1}};
    for (s32 frame = 0; frame < 300; ++frame) {
        visitor.position = Vec3{scene.worldTransform(688) * Vec4{local, 1}};
        world.updateTriggers(1.0f / 30, std::array{visitor});
        world.update(1.0f / 30);
        const Vec3 at{scene.worldTransform(686)[3]};
        const auto movingCue = areas.update(std::array{at}, MusicAreas::kNoArea, &scene);
        REQUIRE(movingCue.has_value());
        CHECK(movingCue->area == elevator->area);
    }
    const Vec3 center{scene.worldTransform(686)[3]};
    REQUIRE(glm::distance(start, center) > elevator->radius);
    const auto cue = areas.update(std::array{center}, MusicAreas::kNoArea, &scene);
    REQUIRE(cue.has_value());
    CHECK(cue->area == elevator->area);
    CHECK(cue->how == elevator->how);
    const Vec3 outside = center + Vec3{0, elevator->radius + 1, 0};
    CHECK_FALSE(areas.update(std::array{outside}, MusicAreas::kNoArea, &scene).has_value());
}

} // namespace
