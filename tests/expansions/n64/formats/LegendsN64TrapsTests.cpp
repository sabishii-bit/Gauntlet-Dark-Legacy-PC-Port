#include <bit>
#include <filesystem>
#include <map>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Traps.h"

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
    std::vector<u8> placements = std::vector<u8>(116);
    std::vector<u8> templates = std::vector<u8>(100);
    Fixture() {
        put32(placements, 12, 92);
        put16(placements, 42, 1);
        put16(placements, 92, 40);
        put16(placements, 94, 80);
        put16(placements, 96, static_cast<u16>(-120));
        put16(placements, 98, 16);
        put16(placements, 100, 0x1004);
        placements[103] = 3;
        real(placements, 108, 2);
        put32(templates, 0, 1);
        put16(templates, 4, 0x1004);
        put16(templates, 6, 0x2081);
        put32(templates, 8, 0x05000000);
        put16(templates, 12, 90);
        templates[14] = 5;
        templates[15] = 30;
        put32(templates, 16, 0x54465652);
        put32(templates, 20, 0x54465641);
        put32(templates, 32, 0x89);
        put32(templates, 36, 1);
        real(templates, 40, 1);
        real(templates, 44, 3);
    }
    LegendsN64Traps decode() const { return decodeLegendsN64Traps(placements, templates); }
};
} // namespace

TEST_CASE("N64 damaging traps preserve native placement timing and non-solid contact dimensions",
          "[legends][native-traps][formats]") {
    Fixture fixture;
    auto decoded = fixture.decode();
    REQUIRE(decoded.traps.size() == 1);
    CHECK(decoded.warnings.empty());
    const auto& trap = decoded.traps.front();
    CHECK(trap.position == Vec3{10, 20, -30});
    CHECK(trap.yaw == .5f);
    CHECK(trap.minimumPlayers == 3);
    CHECK(trap.scale == 2);
    CHECK(trap.dimensions == Vec3{2, 6, 0});
    CHECK(trap.shape == 1);
    CHECK(trap.idleSequence == "TFVR");
    CHECK(trap.activeSequence == "TFVA");
    CHECK(trap.restFrames == 90);
    CHECK(trap.activeFrames == 30);
    CHECK(trap.damage == 5);
    CHECK(trap.sound == 0x89);
    CHECK(trap.groundProjected);
    fixture.placements[102] = 8;
    CHECK(fixture.decode().traps.front().dimensions == Vec3{8, 24, 0});
}

TEST_CASE("N64 damaging trap decoder rejects corrupt metadata",
          "[legends][native-traps][formats]") {
    Fixture fixture;
    SECTION("truncated placements") {
        fixture.placements.pop_back();
    }
    SECTION("truncated templates") {
        fixture.templates.pop_back();
    }
    SECTION("unknown template") {
        put16(fixture.placements, 100, 0xffff);
    }
    SECTION("non-finite scale") {
        put32(fixture.placements, 108, 0x7fc00000);
    }
    SECTION("invalid dimensions") {
        real(fixture.templates, 40, -1);
    }
    SECTION("non-finite dimensions") {
        put32(fixture.templates, 44, 0x7f800000);
    }
    SECTION("invalid sequence") {
        fixture.templates[16] = 0;
    }
    CHECK_THROWS_AS(fixture.decode(), FormatError);
}

TEST_CASE("Native limb traps retain attachment collision instead of stationary volumes",
          "[legends][native-traps][formats]") {
    Fixture fixture;
    put16(fixture.templates, 6, 0x204a);
    put32(fixture.templates, 36, 5);
    real(fixture.templates, 44, 0);
    const auto decoded = fixture.decode();
    REQUIRE(decoded.traps.size() == 1);
    CHECK(decoded.warnings.empty());
    CHECK(decoded.traps.front().shape == 5);
    CHECK(decoded.traps.front().contactNode == "HITNODE");
    CHECK(decoded.traps.front().dimensions ==
          Vec3{1, 0, 0}); // Art scale2 does not scale the attack.
    fixture.placements[102] = 8;
    CHECK(fixture.decode().traps.front().dimensions == Vec3{1, 0, 0});
}

TEST_CASE(
    "Installed N64 damaging trap corpus identifies stationary and limb traps with stage scaling",
    "[legends][native-traps][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "items/generic/ptrap.rom")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto templates = readFile(root / "items/generic/ptrap.rom");
    const auto program = readFile(root / "_program/game.bin");
    usize maps = 0;
    usize warnings = 0;
    std::map<u16, usize> counts;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        CAPTURE(entry.path());
        const auto decoded = decodeLegendsN64Traps(readFile(entry.path() / "ipr.rom"), templates);
        warnings += decoded.warnings.size();
        ++maps;
        for (const auto& trap : decoded.traps) {
            ++counts[trap.templateKey];
        }
        if (!decoded.traps.empty()) {
            const auto scale = legendsN64TrapScale(program, entry.path().filename().string());
            REQUIRE(scale);
            CHECK(*scale > 0);
        }
    }
    CHECK(maps == 34);
    CHECK(warnings == 0);
    CHECK(counts == std::map<u16, usize>{{0x1001, 237},
                                         {0x1002, 30},
                                         {0x1003, 15},
                                         {0x1004, 232},
                                         {0x1005, 15},
                                         {0x2c06, 6},
                                         {0x2c07, 3}});
    CHECK(legendsN64TrapScale(program, "mount1") == 1.0f);
    CHECK(legendsN64TrapScale(program, "castle1") == 1.125f);
    CHECK_FALSE(legendsN64TrapScale(program, "not-a-level"));
    CHECK_FALSE(legendsN64TrapScale({}, "mount1"));
}
