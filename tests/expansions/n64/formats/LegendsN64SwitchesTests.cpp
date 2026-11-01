#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Switches.h"

namespace {
using namespace gdl;
using namespace gdl::formats;
void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8U);
    bytes.at(at + 1) = static_cast<u8>(value);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}
void real(std::vector<u8>& bytes, usize at, f32 value) {
    put32(bytes, at, std::bit_cast<u32>(value));
}
struct Fixture {
    std::vector<u8> placements = std::vector<u8>(122);
    std::vector<u8> templates = std::vector<u8>(100);
    Fixture() {
        put32(placements, 12, 92);
        put32(placements, 32, 116);
        put16(placements, 42, 1);
        put16(placements, 54, 6);
        put16(placements, 92, 40);
        put16(placements, 94, 80);
        put16(placements, 96, static_cast<u16>(-120));
        put16(placements, 98, static_cast<u16>(-16));
        put16(placements, 100, 0x1202);
        placements[103] = 2;
        real(placements, 108, 2);
        put16(placements, 112, static_cast<u16>(-100));
        put16(placements, 114, 30);
        const std::string name = "ELEV1";
        std::copy(name.begin(), name.end(), placements.begin() + 116);
        put32(templates, 0, 1);
        put16(templates, 4, 0x1202);
        put16(templates, 6, 0x3802);
        put32(templates, 8, 0x02000000);
        put32(templates, 16, 0x54504144); // TPAD
        put32(templates, 20, 0x54504441); // TPDA
        put32(templates, 32, 0x100);
        put32(templates, 36, 1);
        real(templates, 40, .5f);
        real(templates, 44, 1);
    }
    LegendsN64Switches decode() const { return decodeLegendsN64Switches(placements, templates); }
};
} // namespace

TEST_CASE("N64 switch descriptors retain authored identities endpoints and contact",
          "[legends][switches][formats]") {
    Fixture fixture;
    const auto decoded = fixture.decode();
    REQUIRE(decoded.switches.size() == 1);
    CHECK(decoded.warnings.empty());
    const auto& plate = decoded.switches.front();
    CHECK(plate.nativeIndex == 0);
    CHECK(plate.templateKey == 0x1202);
    CHECK(plate.target == "ELEV1");
    CHECK(plate.position == Vec3{10, 20, -30});
    CHECK(plate.yaw == -.5f);
    CHECK(plate.scale == 2);
    CHECK(plate.radius == 1);
    CHECK(plate.height == 2);
    CHECK(plate.minimumPlayers == 2);
    CHECK(plate.firstEndpoint == -100);
    CHECK(plate.secondEndpoint == 30);
    CHECK(plate.mode() == 0x800);
    CHECK(plate.oneShot());
    CHECK(plate.supported);
    CHECK(plate.groundProjected);
    CHECK(plate.idleSequence == "TPAD");
    CHECK(plate.activeSequence == "TPDA");
    fixture.placements[102] = 8;
    const auto hidden = fixture.decode().switches.front();
    CHECK(hidden.hidden());
    CHECK(hidden.radius == 4);
    CHECK(hidden.height == 8);
    CHECK(hidden.supported);
    put16(fixture.templates, 6, 0x3822);
    CHECK_FALSE(fixture.decode().switches.front().groundProjected);
}

TEST_CASE("N64 switches preserve unsupported modes without enabling guessed behavior",
          "[legends][switches][formats]") {
    Fixture fixture;
    SECTION("returning breakaway") {
        put16(fixture.templates, 6, 0x2302);
    }
    SECTION("item trigger") {
        put16(fixture.templates, 6, 0x3602);
    }
    SECTION("non-cylinder") {
        put32(fixture.templates, 36, 3);
    }
    SECTION("attack activation") {
        put32(fixture.templates, 8, 0x04000000);
    }
    const auto result = fixture.decode();
    REQUIRE(result.switches.size() == 1);
    CHECK_FALSE(result.switches[0].supported);
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings.front().find("ELEV1") != std::string::npos);
}

TEST_CASE("N64 switch parser rejects invalid bounded records", "[legends][switches][formats]") {
    Fixture fixture;
    SECTION("truncated header") {
        fixture.placements.resize(91);
    }
    SECTION("truncated templates") {
        fixture.templates.resize(99);
    }
    SECTION("unknown template") {
        put16(fixture.placements, 100, 0xffff);
    }
    SECTION("name outside pool") {
        put32(fixture.placements, 104, 6);
    }
    SECTION("negative target offset") {
        put32(fixture.placements, 104, 0xffffffff);
    }
    SECTION("unterminated name") {
        fixture.placements.back() = 'X';
    }
    SECTION("pool past file") {
        put32(fixture.placements, 32, 0xfffffff0);
    }
    SECTION("non-finite scale") {
        put32(fixture.placements, 108, 0x7fc00000);
    }
    SECTION("negative contact radius") {
        real(fixture.templates, 40, -1);
    }
    CHECK_THROWS_AS(fixture.decode(), FormatError);
}

TEST_CASE("Native switch census resolves Mountain lift and keeps Tower script gates separate",
          "[legends][switches][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(root / "items/generic/ptrap.rom")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto templates = readFile(root / "items/generic/ptrap.rom");
    const auto mountain =
        decodeLegendsN64Switches(readFile(root / "levels/mount1/ipr.rom"), templates);
    REQUIRE(mountain.switches.size() == 2);
    CHECK(mountain.warnings.empty());
    const auto& lift = mountain.switches[0];
    CHECK(lift.nativeIndex == 2);
    CHECK(lift.templateKey == 0x1202);
    CHECK(lift.target == "ELEV1");
    CHECK(lift.position == Vec3{-47.25f, 56, 66.25f});
    CHECK(lift.firstEndpoint == -100);
    CHECK(lift.secondEndpoint == 0);
    CHECK(mountain.switches[1].target == "TRAPW1");
    CHECK(mountain.switches[1].mode() == 0x500);
    CHECK(mountain.switches[1].secondEndpoint == -60);
    const auto tower =
        decodeLegendsN64Switches(readFile(root / "levels/tower2/ipr.rom"), templates);
    CHECK(tower.switches.empty());
    CHECK(tower.warnings.empty());
    usize maps = 0;
    usize switches = 0;
    usize supported = 0;
    usize fading = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        CAPTURE(entry.path());
        const auto decoded =
            decodeLegendsN64Switches(readFile(entry.path() / "ipr.rom"), templates);
        ++maps;
        switches += decoded.switches.size();
        supported += static_cast<usize>(std::ranges::count_if(
            decoded.switches, [](const auto& value) { return value.supported; }));
        for (const auto& plate : decoded.switches) {
            if (plate.mode() == 0x100) {
                ++fading;
                CAPTURE(plate.nativeIndex, plate.target);
                CHECK(plate.firstEndpoint == 0);
                CHECK(plate.secondEndpoint == 0);
            }
        }
    }
    CHECK(maps == 34);
    CHECK(switches == 473);
    CHECK(supported == 473);
    CHECK(fading == 24);
}

TEST_CASE("N64 repeat lift and endpoint selectors retain native control modes",
          "[legends][switches][formats]") {
    for (const u16 mode : std::array<u16, 4>{0x100, 0x200, 0x900, 0xa00}) {
        Fixture fixture;
        put16(fixture.templates, 6, static_cast<u16>(0x2002 | mode));
        const auto decoded = fixture.decode();
        REQUIRE(decoded.switches.size() == 1);
        CHECK(decoded.warnings.empty());
        CHECK(decoded.switches[0].supported);
        CHECK_FALSE(decoded.switches[0].oneShot());
        CHECK(decoded.switches[0].mode() == mode);
    }
}
