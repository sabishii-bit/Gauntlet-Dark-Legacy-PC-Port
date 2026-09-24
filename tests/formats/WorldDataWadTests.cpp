#include <bit>
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
using test::ByteWriter;

/** A zeroed record that fields are written into at their offsets. */
struct Record {
    std::vector<u8> bytes;

    explicit Record(usize size) : bytes(size, 0) {}

    void u16At(usize at, u16 value) {
        bytes[at] = static_cast<u8>(value & 0xFFU);
        bytes[at + 1] = static_cast<u8>(value >> 8U);
    }
    void u32At(usize at, u32 value) {
        u16At(at, static_cast<u16>(value & 0xFFFFU));
        u16At(at + 2, static_cast<u16>(value >> 16U));
    }
    void f32At(usize at, f32 value) { u32At(at, std::bit_cast<u32>(value)); }
    void textAt(usize at, std::string_view text) {
        for (const char c : text) {
            bytes.at(at++) = static_cast<u8>(c);
        }
    }
};

/** A realm of one level with its camera, audio and one named sound. */
std::vector<u8> sampleWad() {
    Record world(20);
    world.u32At(0, 13);
    world.textAt(4, "levelL");

    Record level(WorldDataFile::kLevelSize);
    level.u32At(0, 0x10);
    level.textAt(8, "L1");
    level.textAt(0x14, "Tower");
    level.textAt(0x34, "intro");
    level.u32At(0x44, 0xFFFFFFFF);
    level.u16At(0x58, 0);
    level.u16At(0x5A, 0);
    level.u16At(0x5C, 0xFFFF);
    level.bytes[0x70] = 3;
    level.bytes[0x71] = 255;
    level.bytes[0x72] = 200;
    level.bytes[0x73] = 100;
    level.f32At(0x84, 64.0f);
    level.f32At(0x88, 2000.0f);
    level.u16At(0x8E, 25);
    level.f32At(0x94, 0.75f);
    level.f32At(0xA4, 1.25f); // the damage multiplier
    level.f32At(0xA8, 2.0f);  // the difficulty
    level.f32At(0xD8, 1.5f);  // how fast its traps cycle
    level.f32At(0x98, 1.0f);
    level.u32At(0xE0, 1234);
    level.u32At(0xE4, 123);
    level.u32At(0xE8, 2345);
    level.f32At(0xEC, 0.8f);
    level.f32At(0xF0, -1.0f);
    level.f32At(0xF4, -6.0f);
    level.f32At(0xF8, 2.0f);
    level.f32At(0xFC, 1.0f);
    level.f32At(0x100, 0.9f);
    level.f32At(0x104, 0.8f);
    level.f32At(0x108, 1.0f);

    Record camera(WorldDataFile::kCameraSize);
    camera.u16At(2, 1);
    camera.f32At(4, 1.0f);
    camera.f32At(8, 0.6981317f);
    camera.f32At(0x18, 10.0f);
    camera.bytes[0x25] = 255;
    camera.u16At(0x26, 0xFFFF);
    camera.f32At(0x28, 50.0f);
    camera.f32At(0x2C, 24.0f);
    camera.f32At(0x30, 32.0f);
    camera.u16At(0x34, 25);
    camera.f32At(0x58, 0.01f);
    camera.f32At(0x5C, -std::numbers::pi_v<f32>);
    camera.f32At(0x60, std::numbers::pi_v<f32>);

    Record audio(WorldDataFile::kAudioSize);
    audio.textAt(0, "WIZTOWER");
    audio.u16At(0x10, 0);
    audio.u16At(0x12, 1);
    audio.u32At(0x14, 0xFFFFFFFF);
    audio.textAt(0x18, "tower");
    audio.u16At(0x28, 1);
    audio.u16At(0x2A, 1);

    Record sound(WorldDataFile::kSoundSize);
    sound.textAt(0, "S_ENTERING1A");
    sound.u16At(0x14, 5);
    sound.u16At(0x16, 2);

    constexpr u32 kDataAt = 16;
    const u32 worldAt = kDataAt;
    const u32 levelAt = worldAt + 20;
    const u32 cameraAt = levelAt + WorldDataFile::kLevelSize;
    const u32 audioAt = cameraAt + WorldDataFile::kCameraSize;
    const u32 soundAt = audioAt + WorldDataFile::kAudioSize;
    const u32 directoryAt = soundAt + WorldDataFile::kSoundSize;

    ByteWriter w;
    w.putU32(directoryAt).putU32(5).putZeros(8);
    w.putBytes(world.bytes);
    w.putBytes(level.bytes);
    w.putBytes(camera.bytes);
    w.putBytes(audio.bytes);
    w.putBytes(sound.bytes);
    const auto entry = [&](std::string_view reversedTag, u32 offset, u32 count) {
        w.putText(reversedTag).putU32(offset).putU32(count).putU32(count);
    };
    entry("DLRW", worldAt, 1);
    entry("LVEL", levelAt, 1);
    entry("SMAC", cameraAt, 1);
    entry("SDUA", audioAt, 1);
    entry("SDNS", soundAt, 1);
    return w.bytes();
}

TEST_CASE("a wad directory lists its sections with their tags the right way round",
          "[formats][wad]") {
    const std::vector<u8> bytes = sampleWad();
    const std::vector<WadSection> sections = readWadDirectory(bytes, "sample");
    REQUIRE(sections.size() == 5);
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

TEST_CASE("world data wads describe a realm's levels, cameras, audio and sounds",
          "[formats][wad][world]") {
    const WorldDataFile data = WorldDataFile::parse(sampleWad());
    REQUIRE(data.realm == 13);
    REQUIRE(data.prefix == "levelL");
    REQUIRE(data.levels.size() == 1);
    const LevelRecord& level = data.levels[0];
    REQUIRE(level.flags == 0x10);
    REQUIRE(level.name == "L1");
    REQUIRE(level.title == "Tower");
    REQUIRE(level.movie == "intro");
    REQUIRE(level.bossType == -1);
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
    bytes[bytes.size() - usize{5} * 16] = 'X';
    REQUIRE_THROWS_AS(WorldDataFile::parse(bytes), FormatError);
    bytes = sampleWad();
    bytes[bytes.size() - usize{4} * 16 + 8] = 200; // two hundred levels
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
