#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldData.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

std::filesystem::path sampleRealm(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "TOWER.json", R"({
  "realm": 13, "prefix": "levelL",
  "levels": [
    {"name": "L1", "title": "Tower", "cameraIndex": 0, "audioIndex": 0, "musicVolume": 0.75,
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

TEST_CASE("Wraith music metadata names a two-part single-area stream",
          "[assets][world][unpacked][wraith]") {
    const auto path = test::unpackedOrSkip("wdata/DREAM.json");
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

TEST_CASE("the unpacked tower realm carries its light and camera", "[assets][world][unpacked]") {
    WorldData data;
    const auto path = test::unpackedOrSkip("wdata/TOWER.json");
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
