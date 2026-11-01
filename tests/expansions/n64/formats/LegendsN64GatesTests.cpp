#include <algorithm>
#include <bit>
#include <filesystem>
#include <map>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Gates.h"

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
        put16(placements, 100, 0x803);
        placements[103] = 2;
        real(placements, 108, 2);
        put32(templates, 0, 1);
        put16(templates, 4, 0x803);
        put16(templates, 6, 0x1004);
        put32(templates, 8, 0x02000000);
        put32(templates, 16, 0x44443143);
        put32(templates, 20, 0x4444314f);
        put32(templates, 32, 0x100);
        put32(templates, 36, 3);
        real(templates, 40, 3.5f);
        real(templates, 44, 5);
        real(templates, 48, .5f);
        put32(templates, 52, 3);
        real(templates, 56, 1.5f);
        real(templates, 60, 2.1f);
        real(templates, 64, .5f);
        real(templates, 68, 3);
        put32(templates, 76, 5);
    }
    LegendsN64Gates decode() const { return decodeLegendsN64Gates(placements, templates); }
};
} // namespace

TEST_CASE("N64 key gates preserve complete native primary and frame placement",
          "[legends][gates][formats]") {
    Fixture fixture;
    const auto result = fixture.decode();
    REQUIRE(result.gates.size() == 1);
    CHECK(result.warnings.empty());
    const auto& gate = result.gates[0];
    CHECK(gate.nativeIndex == 0);
    CHECK(gate.templateKey == 0x803);
    CHECK(gate.minimumPlayers == 2);
    CHECK(gate.position == Vec3{10, 20, -30});
    CHECK(gate.yaw == .5f);
    CHECK(gate.scale == 2);
    CHECK(gate.groundProjected);
    CHECK(gate.idleSequence == "DD1C");
    CHECK(gate.openingSequence == "DD1O");
    CHECK(gate.sound == 0x100);
    CHECK(gate.primary.shape == 3);
    CHECK(gate.primary.dimensions == Vec3{7, 10, 1});
    CHECK(gate.frames[0].dimensions.x == 3);
    CHECK(gate.frames[0].offset == Vec2{3, 0}); // Native offset is deliberately unscaled.
    CHECK(gate.frames[1].shape == 5);
    fixture.placements[102] = 8;
    CHECK(fixture.decode().gates[0].primary.dimensions == Vec3{28, 40, 4});
}

TEST_CASE("N64 key gate parser rejects corrupt metadata rather than granting access",
          "[legends][gates][formats]") {
    Fixture fixture;
    SECTION("truncated placement") {
        fixture.placements.resize(115);
    }
    SECTION("truncated template") {
        fixture.templates.resize(99);
    }
    SECTION("missing template") {
        put16(fixture.placements, 100, 0xffff);
    }
    SECTION("invalid scale") {
        put32(fixture.placements, 108, 0x7fc00000);
    }
    SECTION("invalid collider") {
        real(fixture.templates, 40, -1);
    }
    SECTION("invalid shape") {
        put32(fixture.templates, 36, 4);
    }
    SECTION("invalid frame offset") {
        put32(fixture.templates, 68, 0x7f800000);
    }
    CHECK_THROWS_AS(fixture.decode(), FormatError);
}

TEST_CASE("Unknown N64 gate activation remains explicitly unsupported",
          "[legends][gates][formats]") {
    Fixture fixture;
    put32(fixture.templates, 8, 0x04000000);
    const auto result = fixture.decode();
    CHECK(result.gates.empty());
    REQUIRE(result.warnings.size() == 1);
}

TEST_CASE("All native N64 key doors decode separately from Tower realm barriers",
          "[legends][gates][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "items/generic/ptrap.rom")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto templates = readFile(root / "items/generic/ptrap.rom");
    std::map<u16, usize> types;
    usize maps = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        CAPTURE(entry.path());
        const auto decoded = decodeLegendsN64Gates(readFile(entry.path() / "ipr.rom"), templates);
        REQUIRE(decoded.warnings.empty());
        for (const auto& gate : decoded.gates) {
            ++types[gate.templateKey];
            CHECK(gate.primary.shape == 3);
            CHECK_FALSE(gate.idleSequence.empty());
            CHECK_FALSE(gate.openingSequence.empty());
        }
        ++maps;
    }
    CHECK(maps == 34);
    CHECK(types == std::map<u16, usize>{{0x801, 73}, {0x802, 3}, {0x803, 5}, {0x804, 13}});
    CHECK(decodeLegendsN64Gates(readFile(root / "levels/tower2/ipr.rom"), templates).gates.empty());
}
