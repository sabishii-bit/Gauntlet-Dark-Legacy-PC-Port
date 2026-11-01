#include <array>
#include <bit>
#include <filesystem>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Camera.h"

namespace {
using namespace gdl;
using namespace gdl::formats;
using Catch::Approx;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8);
    bytes.at(at + 1) = static_cast<u8>(value);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16));
    put16(bytes, at + 2, static_cast<u16>(value));
}

std::vector<u8> settings() {
    std::vector<u8> bytes(92);
    put32(bytes, 0, 76);
    put32(bytes, 4, 92);
    put16(bytes, 8, 2);
    put16(bytes, 76, static_cast<u16>(-7));
    put16(bytes, 78, 19);
    put16(bytes, 80, 100);
    bytes[82] = 128;
    bytes[83] = 64;
    bytes[90] = 64;
    bytes[91] = static_cast<u8>(-32);
    return bytes;
}

std::vector<u8> program() {
    constexpr usize kTable = 0xe006eedcU - 0xe0000400U;
    std::vector<u8> bytes(kTable + 96);
    constexpr std::array<u32, 5> kIds{0, 3, 7, 6, 12};
    for (usize i = 0; i < kIds.size(); ++i) {
        put32(bytes, kTable + i * 16, kIds[i]);
        put32(bytes, kTable + i * 16 + 4, std::bit_cast<u32>(10.0f + static_cast<f32>(i)));
        put32(bytes, kTable + i * 16 + 8, std::bit_cast<u32>(2.0f));
        put32(bytes, kTable + i * 16 + 12, std::bit_cast<u32>(-15.0f));
    }
    put32(bytes, kTable + 80, 16);
    return bytes;
}

TEST_CASE("N64 camera decodes native signed IST markers and executable tower directions",
          "[legends][camera][formats]") {
    const auto ordinary = decodeLegendsN64Camera(settings(), "mount1");
    REQUIRE(ordinary.markers.size() == 2);
    CHECK_FALSE(ordinary.tower);
    CHECK(ordinary.towerExits.empty());
    CHECK(ordinary.markers[0].position == Vec3{-1.75f, 4.75f, 25});
    CHECK(ordinary.markers[0].yaw == Approx(-kPi));
    CHECK(ordinary.markers[0].pitch == Approx(kPi / 4));
    CHECK(ordinary.markers[1].yaw == Approx(kPi / 2));
    CHECK(ordinary.markers[1].pitch == Approx(-kPi / 8));
    const auto tower = decodeLegendsN64Camera(settings(), "TOWER2", program());
    CHECK(tower.tower);
    REQUIRE(tower.towerExits.size() == 5);
    CHECK(tower.towerExits.front() == Vec3{10, 2, -15});
    CHECK(tower.towerExits.back() == Vec3{14, 2, -15});
}

TEST_CASE("N64 camera rejects incomplete and unsupported native records",
          "[legends][camera][formats]") {
    auto bytes = settings();
    SECTION("header") {
        bytes.resize(75);
    }
    SECTION("table before header") {
        put32(bytes, 0, 12);
    }
    SECTION("huge table") {
        put16(bytes, 8, 0xffff);
    }
    SECTION("only opening camera") {
        put16(bytes, 8, 1);
    }
    SECTION("table outside file") {
        put32(bytes, 0, 0xfffffff0U);
    }
    SECTION("truncated last marker") {
        bytes.pop_back();
    }
    CHECK_THROWS_AS(decodeLegendsN64Camera(bytes, "mount1"), FormatError);
}

TEST_CASE("N64 boss camera uses the script arena focus rather than a floor marker",
          "[legends][camera][formats][boss]") {
    auto bytes = program();
    constexpr usize kPose = 0xe006c190U - 0xe0000400U;
    put32(bytes, kPose + 12, std::bit_cast<u32>(-4.0f));
    put32(bytes, kPose + 16, std::bit_cast<u32>(47.0f));
    put32(bytes, kPose + 20, std::bit_cast<u32>(-16.0f));
    const auto dragon = decodeLegendsN64Camera(settings(), "mount6", bytes);
    REQUIRE(dragon.bossFocus);
    CHECK(*dragon.bossFocus == Vec3{-4, 47, -16});
    put32(bytes, kPose + 24 + 16, std::bit_cast<u32>(-5.0f));
    const auto yeti = decodeLegendsN64Camera(settings(), "ice5", bytes);
    REQUIRE(yeti.bossFocus);
    CHECK(yeti.bossFocus->y == 3);
    CHECK_FALSE(decodeLegendsN64Camera(settings(), "mount1").bossFocus);
    CHECK_THROWS_AS(decodeLegendsN64Camera(settings(), "mount6"), FormatError);
    put32(bytes, kPose + 16, std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
    CHECK_THROWS_AS(decodeLegendsN64Camera(settings(), "mount6", bytes), FormatError);
}

TEST_CASE("N64 tower refuses missing or corrupt return direction tables",
          "[legends][camera][formats]") {
    constexpr usize kTable = 0xe006eedcU - 0xe0000400U;
    auto bytes = program();
    SECTION("missing program") {
        bytes.clear();
    }
    SECTION("truncated sentinel") {
        bytes.resize(kTable + 82);
    }
    SECTION("foreign table layout") {
        put32(bytes, kTable, 9);
    }
    SECTION("missing terminator") {
        put32(bytes, kTable + 80, 3);
    }
    SECTION("zero direction") {
        put32(bytes, kTable + 4, 0);
        put32(bytes, kTable + 12, 0);
    }
    SECTION("nonfinite direction") {
        put32(bytes, kTable + 4, std::bit_cast<u32>(std::numeric_limits<f32>::infinity()));
    }
    CHECK_THROWS_AS(decodeLegendsN64Camera(settings(), "tower2", bytes), FormatError);
}

TEST_CASE("N64 camera tables decode across the installed native map catalog",
          "[legends][camera][formats][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_directory(root)) {
        SKIP("Native N64 extraction is not installed");
    }
    const auto executable = readFile(root / "_program/game.bin");
    usize maps = 0;
    usize markers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        INFO(entry.path().string());
        const auto data = decodeLegendsN64Camera(readFile(entry.path() / "ist.rom"),
                                                 entry.path().filename().string(), executable);
        REQUIRE(data.markers.size() >= 2);
        markers += data.markers.size();
        if (entry.path().filename() == "tower2") {
            CHECK(data.markers.size() == 34);
            REQUIRE(data.towerExits.size() == 5);
            CHECK(data.towerExits[0] == Vec3{90, 2, -52});
            CHECK(data.towerExits[4] == Vec3{0, -5, 104});
            CHECK(data.markers[23].position == Vec3{0, .75f, -33.75f});
            CHECK(data.markers[23].yaw == Approx(-kPi));
        }
        ++maps;
    }
    CHECK(maps == 34);
    CHECK(markers == 1959);
}
} // namespace
