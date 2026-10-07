#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldData.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/WorldDataWad.h"

namespace {

using namespace gdl;
using Catch::Approx;

TEST_CASE("gameplay enemy limits follow the camera record rather than the initialization cap",
          "[world-data][generator-population]") {
    const auto dir = test::scratchDirectory("gameplay-enemy-limits");
    writeTextFile(dir / "world.json", R"({
      "levels":[{"name":"G1","cameraIndex":0,"maxEnemies":13},
                {"name":"J4","cameraIndex":1,"maxEnemies":25},
                {"name":"OLD","cameraIndex":2,"maxEnemies":17},
                {"name":"NONE","cameraIndex":-1,"maxEnemies":11}],
      "cameras":[{"enemyMax":25},{"enemyMax":20},{}]
    })");
    WorldData world;
    REQUIRE(world.load(dir / "world.json"));
    REQUIRE(world.level("G1"));
    REQUIRE(world.level("J4"));
    REQUIRE(world.level("OLD"));
    REQUIRE(world.level("NONE"));
    CHECK(world.level("G1")->maxEnemies == 25);
    CHECK(world.level("J4")->maxEnemies == 20);
    CHECK(world.level("OLD")->maxEnemies == 17);
    CHECK(world.level("NONE")->maxEnemies == 11);
}

TEST_CASE("every native realm preserves its gameplay camera enemy capacity",
          "[world-data][generator-population][assets]") {
    constexpr std::array kRealms{"BATTLE", "CASTLE", "DESERT", "DREAM",  "FOREST", "HELL",  "ICE",
                                 "MOUNT",  "SECRET", "SKY",    "TEMPLE", "TEST",   "TOWER", "TOWN"};
    usize checked = 0;
    for (const auto* realm : kRealms) {
        const auto path = test::assetOrSkip(std::string{"WDATA/"} + realm + ".WAD");
        const auto raw = formats::WorldDataFile::parse(readFile(path));
        WorldData world;
        REQUIRE(world.load(path));
        for (const auto& source : raw.levels) {
            CAPTURE(realm, source.name, source.maxEnemies, source.cameraIndex);
            REQUIRE(source.cameraIndex >= 0);
            REQUIRE(static_cast<usize>(source.cameraIndex) < raw.cameras.size());
            const auto limit = raw.cameras[static_cast<usize>(source.cameraIndex)].enemyMax;
            const auto* level = world.level(source.name);
            REQUIRE(level);
            // camera_mode_level (80026CF0) overwrites gNumEnemies from CAMS+0x34
            // after InitEnemies has used LEVL+0x8E. The gameplay limit is not 13 in G1.
            CHECK(level->maxEnemies == limit);
            if (source.name == "G1") {
                CHECK(source.maxEnemies == 13);
                CHECK(limit == 25);
            }
            if (source.name == "J4" || source.name == "J6") {
                CHECK(limit == 20);
            }
            ++checked;
        }
    }
    CHECK(checked == 65);
}

TEST_CASE("camera limit metadata preserves legacy authored boxes and native world derivation",
          "[world-data][camera-bounds]") {
    const auto dir = test::scratchDirectory("world-camera-limits");
    writeTextFile(dir / "world.json", R"({
      "levels":[{"name":"S8","cameraIndex":0}],
      "cameras":[{"limits":0,"boundsMin":[-32,0,-68],"boundsMax":[75,30,26]},
                 {"limits":1},{"boundsMax":[10,20,30]}]
    })");
    WorldData world;
    REQUIRE(world.load(dir / "world.json"));
    REQUIRE(world.camera(0));
    REQUIRE(world.camera(1));
    REQUIRE(world.camera(2));
    CHECK_FALSE(world.camera(0)->authoredBounds);
    // Keep the raw values: only the loaded level can derive the runtime box.
    CHECK(world.camera(0)->boundsMax == Vec3{75, 30, 26});
    CHECK(world.camera(1)->authoredBounds);
    CHECK(world.camera(2)->authoredBounds);
    CHECK(world.camera(2)->boundsMax == Vec3{10, 20, 30});
}

TEST_CASE("level enemy rosters preserve audio aliases separately from model kinds",
          "[world-data][enemy-feedback]") {
    const auto dir = test::scratchDirectory("world-enemy-audio");
    writeTextFile(dir / "world.json", R"({
      "enemies": [{"kind":13,"subtype":12,"stream":"EGRUNT","form":"egr"},
                  {"kind":16,"subtype":12}],
      "levels": [{"name":"E1","enemyTypes":[1,0,-1]}]
    })");
    WorldData world;
    REQUIRE(world.load(dir / "world.json"));
    const auto* level = world.level("E1");
    REQUIRE(level != nullptr);
    REQUIRE(level->enemies.size() == 2);
    CHECK(level->enemies[0].kind == 16);
    CHECK(level->enemies[0].stream.empty());
    CHECK(level->enemies[0].form.empty());
    CHECK(level->enemies[1].kind == 13);
    CHECK(level->enemies[1].subtype == 12);
    CHECK(level->enemies[1].stream == "EGRUNT");
    CHECK(level->enemies[1].form == "egr");
}

std::filesystem::path sampleRealm(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "TOWER.json", R"({
  "realm": 13, "prefix": "levelL",
  "levels": [
    {"name": "L1", "title": "Tower", "selectionFlags": 2, "flags": 4, "timeLimit": 45, "cameraIndex": 0, "audioIndex": 0, "musicVolume": 0.75,
     "tuning": {"playerLevel": 10, "experience": 2.5, "damage": 0, "difficulty": 2,
                "trapRate": 4, "trapDamage": 0, "enemyHealth": 0.75, "enemySpeed": 0,
                "generatorMost": 0.5},
     "maxEnemies": 13, "shopMaxima": [1500, 300, 2500],
     "ambient": 0.8, "lightDirection": [-1, -6, 2], "lightColor": [1, 0.9, 0.8],
     "lightIntensity": 1},
    {"name": "L2", "title": "Tower", "cameraIndex": 5, "audioIndex": -1}
  ],
  "cameras": [{"minPitch": 0.6981317, "boundsMin": [0, 0, 0], "boundsMax": [10, 0, 0],
               "attention": 50, "radiusMin": 24, "radiusMax": 32, "smooth": 0.01}],
  "audio": [{"bank": "WIZTOWER", "stream": "tower", "enterSound": 0, "hitSound": 1}]
})");
    return dir / "TOWER.json";
}

TEST_CASE("world data names a realm's levels and the records they point at", "[assets][world]") {
    WorldData data;
    REQUIRE_FALSE(data.loaded());
    REQUIRE(data.load(sampleRealm("world-data")));
    REQUIRE(data.loaded());
    REQUIRE(data.realm() == 13);
    REQUIRE(data.prefix() == "levelL");
    REQUIRE(data.levels().size() == 2);
    const LevelInfo* level = data.level("L1");
    REQUIRE(level != nullptr);
    REQUIRE(level->title == "Tower");
    REQUIRE(level->selectionFlags == 2);
    CHECK(level->flags == 4);
    CHECK(level->timeLimit == 45);
    CHECK(data.level("L2")->timeLimit == 0);
    REQUIRE(data.level("L2")->selectionFlags == 0);
    REQUIRE(level->musicVolume == Approx(0.75f));
    REQUIRE(level->shopMaxima == std::array<s32, 3>{1500, 300, 2500});
    REQUIRE(data.level("L2")->shopMaxima == std::array<s32, 3>{1000, 100, 1000});
    // A scale left at zero is the level's difficulty; the damage multiplier is then one.
    REQUIRE(level->tuning.difficulty == 2.0f);
    REQUIRE(level->tuning.damage == 1.0f);
    REQUIRE(level->tuning.trapDamage == 2.0f);
    REQUIRE(level->tuning.trapDamageScale(1.5f) == 3.0f);
    REQUIRE(level->tuning.trapTimeScale(1.0f) == 0.25f); // four times as fast
    REQUIRE(level->tuning.trapTimeScale(0.5f) == 0.5f);
    REQUIRE(LevelTuning{}.trapTimeScale(1.0f) == 1.0f);
    // What is won there is scaled by the place, less for a character past what it is for.
    REQUIRE(level->tuning.experienceScale(4) == 2.5f);
    REQUIRE(level->tuning.experienceScale(10) == 2.5f);
    REQUIRE(level->tuning.experienceScale(20) == Approx(1.25f)); // ten levels past: halved
    REQUIRE(LevelTuning{}.experienceScale(50) == 1.0f);
    // The enemies' and generators' scales: what they take and deal is the level's own, the
    // rest grows with the difficulty setting.
    REQUIRE(level->tuning.enemyHealth == 0.75f);
    REQUIRE(level->tuning.enemySpeed == 2.0f);
    REQUIRE(level->tuning.enemySpeedScale(1.5f) == 3.0f);
    REQUIRE(level->tuning.enemySightScale(0.5f) == 1.0f);
    REQUIRE(level->tuning.generatorMostScale(2.0f) == 1.0f);
    REQUIRE(level->tuning.generatorRateScale(1.0f) == 2.0f);
    REQUIRE(level->maxEnemies == 13);
    REQUIRE(level->bossType == -1);
    REQUIRE(level->ambient == Approx(0.8f));
    REQUIRE(level->lightDirection == Vec3{-1.0f, -6.0f, 2.0f});
    REQUIRE(level->lightColor.y == Approx(0.9f));
    REQUIRE(data.level("L9") == nullptr);
    const LevelCameraInfo* camera = data.camera(level->cameraIndex);
    REQUIRE(camera != nullptr);
    REQUIRE(camera->radiusMin == 24.0f);
    REQUIRE(camera->radiusMax == 32.0f);
    REQUIRE(camera->minPitch == Approx(0.6981317f));
    REQUIRE(camera->boundsMax.x == 10.0f);
    const LevelAudioInfo* audio = data.audio(level->audioIndex);
    REQUIRE(audio != nullptr);
    REQUIRE(audio->bank == "WIZTOWER");
    REQUIRE(audio->stream == "tower");
    REQUIRE(audio->hitSound == 1);
    REQUIRE(audio->areas == 1);
    REQUIRE(audio->parts[0] == 0); // legacy manifests without part metadata remain usable
    // The second level points past the records it has.
    const LevelInfo* second = data.level("L2");
    REQUIRE(second != nullptr);
    REQUIRE(second->ambient == 1.0f); // the default when unspecified
    REQUIRE(data.camera(second->cameraIndex) == nullptr);
    REQUIRE(data.audio(second->audioIndex) == nullptr);
}

TEST_CASE("Wraith music metadata names a two-part single-area stream", "[assets][world][wraith]") {
    const auto path = test::assetOrSkip("WDATA/DREAM.WAD");
    WorldData data;
    REQUIRE(data.load(path));
    const auto* level = data.level("J5");
    REQUIRE(level != nullptr);
    const auto* audio = data.audio(level->audioIndex);
    REQUIRE(audio != nullptr);
    REQUIRE(audio->stream == "dream5");
    REQUIRE(audio->areas == 1);
    REQUIRE(audio->parts[0] == 2);
}

TEST_CASE("missing or malformed world data fails to load", "[assets][world]") {
    WorldData data;
    REQUIRE_FALSE(data.load(test::scratchDirectory("world-data-none") / "NONE.json"));
    const auto dir = test::scratchDirectory("world-data-bad");
    writeTextFile(dir / "BAD.json", R"({"realm": 1})");
    REQUIRE_FALSE(data.load(dir / "BAD.json"));
    REQUIRE_FALSE(data.loaded());
}

TEST_CASE("the native tower realm carries its light and camera", "[assets][world]") {
    WorldData data;
    const auto path = test::assetOrSkip("WDATA/TOWER.WAD");
    REQUIRE(data.load(path));
    const LevelInfo* level = data.level("L1");
    REQUIRE(level != nullptr);
    REQUIRE(level->ambient == Approx(0.8f));
    REQUIRE(level->lightDirection == Vec3{-1.0f, -6.0f, 2.0f});
    const LevelCameraInfo* camera = data.camera(level->cameraIndex);
    REQUIRE(camera != nullptr);
    REQUIRE(camera->radiusMin == 24.0f);
    REQUIRE(data.audio(level->audioIndex)->stream == "tower");
    // The realm names the sounds its levels enter and hit with.
    REQUIRE(data.soundName(data.audio(level->audioIndex)->enterSound) == "S_ENTERING1A");
    REQUIRE(data.soundName(-1).empty());
    REQUIRE(data.soundName(99).empty());
}

} // namespace
