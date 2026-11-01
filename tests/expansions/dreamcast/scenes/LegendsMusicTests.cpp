#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/scenes/LegendsMusic.h"

namespace {
using namespace gdl;
using namespace gdl::game;
void put(std::vector<u8>& bytes, usize at, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes.at(at + i) = static_cast<u8>(value >> (i * 8));
    }
}
std::vector<u8> worlddata(std::string_view stream = "music") {
    std::vector<u8> bytes(236);
    put(bytes, 0, 212);
    put(bytes, 4, 2);
    put(bytes, 212, 0x41554453);
    put(bytes, 216, 8);
    put(bytes, 220, 1);
    put(bytes, 224, 0x4c45564c);
    put(bytes, 228, 68);
    put(bytes, 232, 1);
    std::copy(stream.begin(), stream.end(), bytes.begin() + 32);
    put(bytes, 48, 1);
    bytes[76] = 'A';
    bytes[77] = '1';
    return bytes;
}
} // namespace

TEST_CASE("Dreamcast level music follows native WDATA references only", "[legends][audio][music]") {
    const auto root = test::scratchDirectory("dreamcast-native-music");
    const auto level = root / "native/levels/levela1";
    std::filesystem::create_directories(root / "native/worlddata");
    std::filesystem::create_directories(root / "disc/AUDIO/STREAMS");
    auto bytes = worlddata();
    bool expected = true;
    SECTION("exact match") {}
    SECTION("missing stream has no substitute") {
        bytes[32] = 'x';
        expected = false;
    }
    SECTION("different level has no realm inference") {
        bytes[77] = '2';
        expected = false;
    }
    SECTION("multiple areas need native zone selection") {
        put(bytes, 48, 2);
        expected = false;
    }
    SECTION("unsafe native name is not a path") {
        bytes[32] = '/';
        expected = false;
    }
    writeFile(root / "native/worlddata/test.wad", bytes);
    writeTextFile(root / "disc/AUDIO/STREAMS/MUSIC.STR", "test only");
    const auto result = dreamcastLegendsMusic(level);
    CHECK(result.has_value() == expected);
    if (result) {
        CHECK(*result == root / "disc/AUDIO/STREAMS/MUSIC.STR");
        CHECK(dreamcastLegendsMusic(level / "") == result);
    }
}

TEST_CASE("Dreamcast music lookup rejects malformed WDATA extents", "[legends][audio][music]") {
    const auto root = test::scratchDirectory("dreamcast-native-music-bad");
    std::filesystem::create_directories(root / "native/worlddata");
    std::filesystem::create_directories(root / "disc/AUDIO/STREAMS");
    auto bytes = worlddata();
    SECTION("directory extent") {
        put(bytes, 4, 0xffffffffU);
    }
    SECTION("audio extent") {
        put(bytes, 220, 100);
    }
    SECTION("level extent") {
        put(bytes, 232, 100);
    }
    SECTION("audio index") {
        bytes[142] = 10;
    }
    writeFile(root / "native/worlddata/test.wad", bytes);
    CHECK_THROWS_AS(dreamcastLegendsMusic(root / "native/levels/levela1"), FormatError);
}

TEST_CASE("Dreamcast music selects later audio records rather than runtime pointer words",
          "[legends][audio][music]") {
    const auto root = test::scratchDirectory("dreamcast-native-music-index");
    std::filesystem::create_directories(root / "native/worlddata");
    std::filesystem::create_directories(root / "disc/AUDIO/STREAMS");
    auto bytes = worlddata();
    bytes.insert(bytes.begin() + 68, 60, 0);
    put(bytes, 0, 272);
    put(bytes, 280, 2);
    put(bytes, 288, 128);
    const std::string second = "second";
    std::copy(second.begin(), second.end(), bytes.begin() + 92);
    put(bytes, 108, 1);
    bytes[128 + 74] = 1;
    // The old GameCube +90 layout reads this zero and incorrectly plays MUSIC.
    writeFile(root / "native/worlddata/test.wad", bytes);
    writeTextFile(root / "disc/AUDIO/STREAMS/MUSIC.STR", "test only");
    writeTextFile(root / "disc/AUDIO/STREAMS/SECOND.STR", "test only");
    const auto track = dreamcastLegendsMusic(root / "native/levels/levela1");
    REQUIRE(track);
    CHECK(track->filename() == "SECOND.STR");
}

TEST_CASE("Dreamcast installed levels resolve exact native soundtrack files",
          "[legends][audio][music][assets]") {
    const auto levels =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::exists(levels)) {
        SKIP("Dreamcast native extraction is not installed");
    }
    usize mapped = 0;
    for (const auto& entry : std::filesystem::directory_iterator(levels)) {
        const auto world = entry.path() / "worlds.rom";
        if (!std::filesystem::exists(world) || std::filesystem::file_size(world) == 0) {
            continue;
        }
        INFO(entry.path().string());
        const auto track = dreamcastLegendsMusic(entry.path());
        const auto name = entry.path().filename().string();
        // C2/D5 require native music-zone selection; S6's stream is absent.
        const bool authored =
            name != "levela2" && name != "levelc2" && name != "leveld5" && name != "levels6";
        REQUIRE(track.has_value() == authored);
        if (track) {
            CHECK(std::filesystem::is_regular_file(*track));
            ++mapped;
        }
    }
    CHECK(mapped == 37);
    CHECK(dreamcastLegendsMusic(levels / "levelt1")->filename() == "TOWER.STR");
    CHECK(dreamcastLegendsMusic(levels / "levela3")->filename() == "CASTLE3.STR");
    CHECK(dreamcastLegendsMusic(levels / "levelb4")->filename() == "MOUNT4.STR");
    CHECK(dreamcastLegendsMusic(levels / "levelg4")->filename() == "TOWN4.STR");
    CHECK(dreamcastLegendsMusic(levels / "levele2")->filename() == "CATH2.STR");
    CHECK(dreamcastLegendsMusic(levels / "levelh5")->filename() == "BATTLE2.STR");
}
