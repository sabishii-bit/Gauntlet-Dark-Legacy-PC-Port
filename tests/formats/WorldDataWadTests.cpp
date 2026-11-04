#include <array>
#include <numbers>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/WadDirectory.h"
#include "formats/WorldDataWad.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using Catch::Approx;
using test::setF32LE;
using test::setText;
using test::setU16LE;
using test::setU32LE;
using test::SyntheticBytes;

TEST_CASE("all nine secret worlds retain their individual retail time limits",
          "[secret][formats][assets]") {
    const auto data = WorldDataFile::parse(readFile(test::assetOrSkip("WDATA/SECRET.WAD")));
    const std::array<s32, 9> seconds{70, 40, 45, 50, 130, 100, 55, 70, 60};
    REQUIRE(data.levels.size() == seconds.size());
    for (usize i = 0; i < seconds.size(); ++i) {
        CHECK(data.levels[i].timeLimit == seconds[i]);
        CHECK((data.levels[i].flags & 4) != 0);
    }
}

TEST_CASE("realm maps preserve their glow and route coordinates",
          "[formats][wad][travel][assets]") {
    const auto data = WorldDataFile::parse(readFile(test::assetOrSkip("WDATA/TOWN.WAD")));
    REQUIRE(data.maps.size() == 5);
    CHECK(data.maps[0][0] == Vec2{36, 164});
    CHECK(data.maps[0][1] == Vec2{-1, -1});
    CHECK(data.maps[1][1] == Vec2{56, 180});
    CHECK(data.levels[0].mapIndex == 0);
}

/** The fixed text fields of a realm's records: a level's name, and every other name. */
constexpr usize kLevelNameField = 4;
constexpr usize kTextField = 16;

/** A realm of one level with its camera, audio and one named sound. */
std::vector<u8> sampleWad() {
    std::vector<u8> world(20, 0);
    setU32LE(world, 0, 13);
    setText(world, 4, kTextField, "levelL");

    std::vector<u8> level(WorldDataFile::kLevelSize, 0);
    setU32LE(level, 0, 0x10);
    setU16LE(level, 4, 2);
    setText(level, 8, kLevelNameField, "L1");
    setU16LE(level, 0x0C, 45);
    setText(level, 0x14, kTextField, "Tower");
    setText(level, 0x34, kTextField, "intro");
    setU32LE(level, 0x44, 0xFFFFFFFF);
    setU32LE(level, 0x48, 1);
    setU16LE(level, 0x58, 0);
    setU16LE(level, 0x5A, 0);
    setU16LE(level, 0x5C, 0xFFFF);
    level[0x70] = 3;
    level[0x71] = 255;
    level[0x72] = 200;
    level[0x73] = 100;
    setF32LE(level, 0x84, 64.0f);
    setF32LE(level, 0x88, 2000.0f);
    setU16LE(level, 0x8E, 25);
    setF32LE(level, 0x94, 0.75f);
    setF32LE(level, 0xA4, 1.25f); // the damage multiplier
    setF32LE(level, 0xA8, 2.0f);  // the difficulty
    setF32LE(level, 0xD8, 1.5f);  // how fast its traps cycle
    setF32LE(level, 0x98, 1.0f);
    setU32LE(level, 0xE0, 1234);
    setU32LE(level, 0xE4, 123);
    setU32LE(level, 0xE8, 2345);
    setF32LE(level, 0xEC, 0.8f);
    setF32LE(level, 0xF0, -1.0f);
    setF32LE(level, 0xF4, -6.0f);
    setF32LE(level, 0xF8, 2.0f);
    setF32LE(level, 0xFC, 1.0f);
    setF32LE(level, 0x100, 0.9f);
    setF32LE(level, 0x104, 0.8f);
    setF32LE(level, 0x108, 1.0f);

    std::vector<u8> camera(WorldDataFile::kCameraSize, 0);
    setU16LE(camera, 2, 1);
    setF32LE(camera, 4, 1.0f);
    setF32LE(camera, 8, 0.6981317f);
    setF32LE(camera, 0x18, 10.0f);
    camera[0x25] = 255;
    setU16LE(camera, 0x26, 0xFFFF);
    setF32LE(camera, 0x28, 50.0f);
    setF32LE(camera, 0x2C, 24.0f);
    setF32LE(camera, 0x30, 32.0f);
    setU16LE(camera, 0x34, 25);
    setF32LE(camera, 0x58, 0.01f);
    setF32LE(camera, 0x5C, -std::numbers::pi_v<f32>);
    setF32LE(camera, 0x60, std::numbers::pi_v<f32>);

    std::vector<u8> audio(WorldDataFile::kAudioSize, 0);
    setText(audio, 0, kTextField, "WIZTOWER");
    setU16LE(audio, 0x10, 0);
    setU16LE(audio, 0x12, 1);
    setU32LE(audio, 0x14, 0xFFFFFFFF);
    setText(audio, 0x18, kTextField, "tower");
    setU16LE(audio, 0x28, 1);
    setU16LE(audio, 0x2A, 1);

    std::vector<u8> sound(WorldDataFile::kSoundSize, 0);
    setText(sound, 0, kTextField, "S_ENTERING1A");
    setU16LE(sound, 0x14, 5);
    setU16LE(sound, 0x16, 2);

    constexpr u32 kDataAt = 16;
    const u32 worldAt = kDataAt;
    const u32 levelAt = worldAt + 20;
    const u32 cameraAt = levelAt + WorldDataFile::kLevelSize;
    const u32 audioAt = cameraAt + WorldDataFile::kCameraSize;
    const u32 soundAt = audioAt + WorldDataFile::kAudioSize;
    const u32 mapAt = soundAt + WorldDataFile::kSoundSize;
    const u32 directoryAt = mapAt + 72;

    SyntheticBytes w;
    w.putU32(directoryAt).putU32(6).putZeros(8);
    w.putBytes(world);
    w.putBytes(level);
    w.putBytes(camera);
    w.putBytes(audio);
    w.putBytes(sound);
    std::vector<u8> map(72, 0);
    for (usize i = 0; i < 9; ++i) {
        setF32LE(map, i * 8, i == 0 ? 36.0f : -1.0f);
        setF32LE(map, i * 8 + 4, i == 0 ? 164.0f : -1.0f);
    }
    w.putBytes(map);
    const auto entry = [&](std::string_view reversedTag, u32 offset, u32 count) {
        w.putText(reversedTag).putU32(offset).putU32(count).putU32(count);
    };
    entry("DLRW", worldAt, 1);
    entry("LVEL", levelAt, 1);
    entry("SMAC", cameraAt, 1);
    entry("SDUA", audioAt, 1);
    entry("SDNS", soundAt, 1);
    entry("SPAM", mapAt, 1);
    return w.bytes();
}

TEST_CASE("a wad directory lists its sections with their tags the right way round",
          "[formats][wad]") {
    const std::vector<u8> bytes = sampleWad();
    const std::vector<WadSection> sections = readWadDirectory(bytes, "sample");
    REQUIRE(sections.size() == 6);
    REQUIRE(sections[0].tag == "WRLD");
    REQUIRE(sections[1].tag == "LEVL");
    REQUIRE(sections[1].offset == 36);
    REQUIRE(sections[1].count == 1);
    REQUIRE(findWadSection(sections, "AUDS") == &sections[3]);
    REQUIRE(findWadSection(sections, "NOPE") == nullptr);
    REQUIRE(readWadText(bytes, sections[0].offset + 4, 16, "sample") == "levelL");
    REQUIRE_THROWS_AS(readWadDirectory(std::vector<u8>(8, 0), "sample"), FormatError);
    std::vector<u8> bad = bytes;
    bad[0] = 0xFF;
    bad[1] = 0xFF;
    REQUIRE_THROWS_AS(readWadDirectory(bad, "sample"), FormatError);
}

TEST_CASE("enemy roster audio and model names occupy separate eight-byte fields",
          "[formats][wad][asset-conformance]") {
    // Deliberately omit a zero terminator from the audio alias: it must not eat
    // the following form. init_next_level reads that second field at +0x10.
    SyntheticBytes writer;
    constexpr u32 kRecord = 16;
    constexpr u32 kWorld = kRecord + WorldDataFile::kEnemySize;
    constexpr u32 kDirectory = kWorld + 20;
    writer.putU32(kDirectory).putU32(2).putZeros(8);
    writer.putU32(32).putU32(5).putText("ABCDEFGH").putText("serp").putZeros(4);
    writer.putU32(2).putText("levelB").putZeros(10);
    writer.putText("YMNE").putU32(kRecord).putU32(1).putU32(1);
    writer.putText("DLRW").putU32(kWorld).putU32(1).putU32(1);
    const auto data = WorldDataFile::parse(writer.bytes());
    REQUIRE(data.enemies.size() == 1);
    CHECK(data.enemies[0].kind == 32);
    CHECK(data.enemies[0].subtype == 5);
    CHECK(data.enemies[0].stream == "ABCDEFGH");
    CHECK(data.enemies[0].form == "serp");
}

TEST_CASE("world data wads describe a realm's levels, cameras, audio and sounds",
          "[formats][wad][world]") {
    const WorldDataFile data = WorldDataFile::parse(sampleWad());
    REQUIRE(data.realm == 13);
    REQUIRE(data.maps.size() == 1);
    CHECK(data.maps[0][0] == Vec2{36, 164});
    CHECK(data.maps[0][8] == Vec2{-1, -1});
    REQUIRE(data.prefix == "levelL");
    REQUIRE(data.levels.size() == 1);
    const LevelRecord& level = data.levels[0];
    REQUIRE(level.flags == 0x10);
    REQUIRE(level.selectionFlags == 2);
    CHECK(level.timeLimit == 45);
    REQUIRE(level.name == "L1");
    REQUIRE(level.title == "Tower");
    REQUIRE(level.movie == "intro");
    REQUIRE(level.bossType == -1);
    REQUIRE(level.earlyEnemies == 1);
    REQUIRE(level.cameraIndex == 0);
    REQUIRE(level.audioIndex == 0);
    REQUIRE(level.mapIndex == -1);
    REQUIRE(level.maxEnemies == 25);
    REQUIRE(level.musicVolume == Approx(0.75f));
    REQUIRE(level.shopMaxima == std::array<s32, 3>{1234, 123, 2345});
    REQUIRE(LevelTuningRecord::kNames[2] == "damage");
    REQUIRE(level.tuning.values[2] == Approx(1.25f));
    REQUIRE(level.tuning.values[3] == Approx(2.0f));
    REQUIRE(LevelTuningRecord::kNames[15] == "trapRate");
    REQUIRE(level.tuning.values[15] == Approx(1.5f));
    REQUIRE(level.tuning.values[16] == 0.0f); // left to the difficulty
    REQUIRE(level.ambient == Approx(0.8f));
    REQUIRE(level.lightDirection == Vec3{-1.0f, -6.0f, 2.0f});
    REQUIRE(level.lightColor.y == Approx(0.9f));
    REQUIRE(level.lightIntensity == 1.0f);
    REQUIRE(level.fog.type == 3);
    REQUIRE(level.fog.color[2] == 100);
    REQUIRE(level.fog.near == 64.0f);
    REQUIRE(level.fog.far == 2000.0f);

    REQUIRE(data.cameras.size() == 1);
    const CameraRecord& camera = data.cameras[0];
    REQUIRE(camera.pitchDirection == 1);
    REQUIRE(camera.minPitch == Approx(0.6981317f));
    REQUIRE(camera.boundsMax.x == 10.0f);
    REQUIRE(camera.startEvent == 255);
    REQUIRE(camera.attentionCamera == -1);
    REQUIRE(camera.attention == 50.0f);
    REQUIRE(camera.radiusMin == 24.0f);
    REQUIRE(camera.radiusMax == 32.0f);
    REQUIRE(camera.enemyMax == 25);
    REQUIRE(camera.smooth == Approx(0.01f));
    REQUIRE(camera.maxYaw == Approx(std::numbers::pi_v<f32>));

    REQUIRE(data.audio.size() == 1);
    REQUIRE(data.audio[0].bank == "WIZTOWER");
    REQUIRE(data.audio[0].stream == "tower");
    REQUIRE(data.audio[0].enterSound == 0);
    REQUIRE(data.audio[0].hitSound == 1);
    REQUIRE(data.audio[0].nameSound == -1);
    REQUIRE(data.audio[0].stereo == 1);

    REQUIRE(data.sounds.size() == 1);
    REQUIRE(data.sounds[0].name == "S_ENTERING1A");
    REQUIRE(data.sounds[0].volume == 5);
    REQUIRE(data.sounds[0].priority == 2);

    // A wad without the world header, or with records past its end, is rejected.
    std::vector<u8> bytes = sampleWad();
    bytes[bytes.size() - usize{6} * 16] = 'X';
    REQUIRE_THROWS_AS(WorldDataFile::parse(bytes), FormatError);
    bytes = sampleWad();
    bytes[bytes.size() - usize{5} * 16 + 8] = 200; // two hundred levels
    REQUIRE_THROWS_AS(WorldDataFile::parse(bytes), FormatError);
    bytes = sampleWad();
    bytes[bytes.size() - 16 + 8] = 200; // two hundred map coordinate records
    REQUIRE_THROWS_AS(WorldDataFile::parse(bytes), FormatError);
}

TEST_CASE("the tower's realm lights its hall from above and keeps the camera within reach",
          "[formats][wad][world][assets]") {
    const auto path = test::assetOrSkip("WDATA/TOWER.WAD");
    const WorldDataFile data = WorldDataFile::parse(readFile(path));
    REQUIRE(data.realm == 13);
    REQUIRE(data.prefix == "levelL");
    REQUIRE(data.levels.size() == 2);
    REQUIRE(data.levels[0].name == "L1");
    REQUIRE(data.levels[0].title == "Tower");
    REQUIRE(data.levels[0].ambient == Approx(0.8f));
    REQUIRE(data.levels[0].lightDirection == Vec3{-1.0f, -6.0f, 2.0f});
    REQUIRE(data.levels[0].lightColor == Vec3{1.0f, 1.0f, 1.0f});
    REQUIRE(data.levels[0].lightIntensity == 1.0f);
    REQUIRE(data.cameras.size() == 1);
    REQUIRE(data.cameras[0].minPitch == Approx(0.6981317f));
    REQUIRE(data.cameras[0].radiusMin == 24.0f);
    REQUIRE(data.cameras[0].radiusMax == 32.0f);
    REQUIRE(data.audio.size() == 1);
    REQUIRE(data.audio[0].bank == "WIZTOWER");
    REQUIRE(data.audio[0].stream == "tower");
    REQUIRE(data.sounds.size() == 2);
    REQUIRE(data.sounds[0].name == "S_ENTERING1A");
}

} // namespace
