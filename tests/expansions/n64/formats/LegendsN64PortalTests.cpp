#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Portal.h"

namespace {
using namespace gdl;
using namespace gdl::formats;
using Catch::Approx;

constexpr usize kRealms = 0xe006c360U - 0xe0000400U;
constexpr usize kLevels = 0xe006c468U - 0xe0000400U;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8U);
    bytes.at(at + 1) = static_cast<u8>(value);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}
void putFloat(std::vector<u8>& bytes, usize at, f32 value) {
    put32(bytes, at, std::bit_cast<u32>(value));
}
void putText(std::vector<u8>& bytes, usize at, std::string_view text) {
    std::ranges::copy(text, bytes.begin() + static_cast<std::ptrdiff_t>(at));
}
std::vector<u8> program() {
    std::vector<u8> bytes(kLevels + 200);
    put32(bytes, kRealms, 0);
    bytes[kRealms + 4] = 'A';
    put32(bytes, kRealms + 8, 0xe0000400U + static_cast<u32>(kLevels + 160));
    put32(bytes, kRealms + 24, 16);
    put32(bytes, kLevels, 0);
    put32(bytes, kLevels + 4, 1); // ordinal1, authored6; must not become castle2
    bytes[kLevels + 8] = '6';
    put32(bytes, kLevels + 12, 0xe0000400U + static_cast<u32>(kLevels + 168));
    put32(bytes, kLevels + 40, 16);
    putText(bytes, kLevels + 160, "castle");
    putText(bytes, kLevels + 168, "6");
    return bytes;
}
std::vector<u8> placements() {
    std::vector<u8> bytes(100);
    put32(bytes, 12, 76);
    put16(bytes, 42, 1);
    put16(bytes, 76, static_cast<u16>(-12));
    put16(bytes, 78, 8);
    put16(bytes, 80, 100);
    put16(bytes, 82, static_cast<u16>(-48));
    put16(bytes, 84, 0x0d01);
    bytes[86] = 8;
    bytes[87] = 3;
    put32(bytes, 88, 0x0106);
    putFloat(bytes, 92, .5f);
    return bytes;
}
std::vector<u8> templates() {
    std::vector<u8> bytes(100);
    put32(bytes, 0, 1);
    put16(bytes, 4, 0x0d01);
    put16(bytes, 6, 5);
    put32(bytes, 8, 0x02000000);
    putText(bytes, 16, "EXNR");
    putText(bytes, 20, "EXNA");
    put32(bytes, 36, 1);
    putFloat(bytes, 40, 2.5f);
    putFloat(bytes, 44, 4);
    return bytes;
}

TEST_CASE("N64 exits retain authored destination and contact metadata",
          "[legends][portal][formats]") {
    const auto decoded = decodeLegendsN64Portals(placements(), templates(), program());
    REQUIRE(decoded.size() == 1);
    const auto& portal = decoded.front();
    CHECK(portal.nativeIndex == 0);
    CHECK(portal.destination == 0x106);
    CHECK(portal.map == "castle6");
    CHECK(portal.position == Vec3{-3, 2, 25});
    CHECK(portal.yaw == Approx(-1.5f));
    CHECK(portal.scale == .5f);
    CHECK(portal.radius == 1.25f);
    CHECK(portal.height == 2);
    CHECK(portal.minimumPlayers == 3);
    CHECK(portal.placementFlags == 8);
    CHECK(portal.idle == "EXNR");
    CHECK(portal.starting == "EXNS");
    CHECK(portal.active == "EXNA");
    CHECK(portal.closing == "EXNE");
}

TEST_CASE("N64 portal destinations refuse guesses and unsafe paths", "[legends][portal][formats]") {
    auto bytes = program();
    CHECK_FALSE(legendsN64Destination(bytes, 0));
    CHECK_FALSE(legendsN64Destination(bytes, 0xffffffd0));
    CHECK_FALSE(legendsN64Destination(bytes, 0x201));
    CHECK_FALSE(legendsN64Destination(bytes, 0x105));
    SECTION("missing program") {
        bytes.clear();
    }
    SECTION("unmapped string pointer") {
        put32(bytes, kRealms + 8, 0xe00003ff);
    }
    SECTION("string outside program") {
        put32(bytes, kRealms + 8, 0xffffffffU);
    }
    SECTION("path traversal") {
        putText(bytes, kLevels + 160, "../bad");
    }
    SECTION("unbounded realm list") {
        for (usize at = kRealms; at < kLevels; at += 24) {
            put32(bytes, at, 0);
        }
    }
    SECTION("invalid level record") {
        put32(bytes, kLevels + 40, 99);
    }
    CHECK_THROWS_AS(legendsN64Destination(bytes, 0x106), FormatError);
}

TEST_CASE("N64 exit decoder rejects malformed contact data", "[legends][portal][formats]") {
    auto instances = placements();
    auto types = templates();
    SECTION("truncated placement") {
        instances.resize(99);
    }
    SECTION("bad placement pointer") {
        put32(instances, 12, 0xfffffff0);
    }
    SECTION("header overlap") {
        put32(instances, 12, 0);
    }
    SECTION("unknown type") {
        put16(instances, 84, 0x1234);
    }
    SECTION("oversized scale") {
        putFloat(instances, 92, 101);
    }
    SECTION("nonfinite scale") {
        putFloat(instances, 92, std::numeric_limits<f32>::infinity());
    }
    SECTION("zero radius") {
        putFloat(types, 40, 0);
    }
    SECTION("nonfinite height") {
        putFloat(types, 44, std::numeric_limits<f32>::quiet_NaN());
    }
    SECTION("unsupported shape") {
        put32(types, 36, 3);
    }
    SECTION("invalid sequence") {
        types[16] = 0;
    }
    SECTION("truncated template") {
        types.resize(99);
    }
    SECTION("unbounded template count") {
        put32(types, 0, 0xffffffffU);
    }
    CHECK_THROWS_AS(decodeLegendsN64Portals(instances, types, program()), FormatError);
}

TEST_CASE("N64 paired transport and secret return are not level exit aliases",
          "[legends][portal][formats]") {
    auto types = templates();
    put16(types, 6, 6);
    CHECK(decodeLegendsN64Portals(placements(), types, {}).empty());
    CHECK(decodeLegendsN64Portals({}, {}, {}).empty());
    auto instances = placements();
    put16(instances, 42, 0);
    CHECK(decodeLegendsN64Portals(instances, {}, {}).empty());
    put16(instances, 42, 1);
    put16(types, 6, 5);
    put32(instances, 88, 0);
    const auto unresolved = decodeLegendsN64Portals(instances, types, {});
    REQUIRE(unresolved.size() == 1);
    CHECK(unresolved.front().map.empty());
    for (const f32 unset : {0.0f, -1.0f, .0001f}) {
        putFloat(instances, 92, unset);
        const auto defaults = decodeLegendsN64Portals(instances, types, {});
        REQUIRE(defaults.size() == 1);
        CHECK(defaults.front().scale == 1);
        CHECK(defaults.front().radius == 2.5f);
    }
}

TEST_CASE("Native N64 Tower has24 authored exits resolved to installed maps",
          "[legends][portal][formats][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto types = readFile(root / "items/generic/ptrap.rom");
    const auto code = readFile(root / "_program/game.bin");
    CHECK(legendsN64ExitDestination(code, 0, "mount1") == "tower2");
    CHECK(legendsN64ExitDestination(code, 0xffffffd0, "castle1") == "tower2");
    CHECK(legendsN64ExitDestination(code, 0x902, "ice1") == "tower2");
    CHECK(legendsN64ExitDestination(code, 0x703, "town2") == "tower2");
    CHECK(legendsN64ExitDestination(code, 0x802, "hell1") == "hell2");
    CHECK(legendsN64ExitDestination(code, 0xf02, "boss1") == "boss2");
    CHECK(legendsN64ExitDestination(code, 0x1302, "mount1") == "secret2");
    CHECK(legendsN64ExitDestination(code, 0x201, "TOWER2") == "mount1");
    CHECK_FALSE(legendsN64ExitDestination(code, 0, "unlisted-map"));
    const auto portals =
        decodeLegendsN64Portals(readFile(root / "levels/tower2/ipr.rom"), types, code);
    REQUIRE(portals.size() == 24);
    const std::array<std::string_view, 24> expected{
        "castle1", "castle6", "castle3", "castle4", "castle5", "mount1",  "mount2",  "mount3",
        "mount4",  "mount5",  "mount6",  "town1",   "town2",   "town3",   "town4",   "town5",
        "ice1",    "ice2",    "ice3",    "ice4",    "ice5",    "battle1", "battle2", "battle3"};
    for (usize i = 0; i < portals.size(); ++i) {
        CAPTURE(i);
        CHECK(portals[i].nativeIndex == i);
        CHECK(portals[i].map == expected[i]);
        CHECK(portals[i].minimumPlayers == 1);
        CHECK(portals[i].placementFlags == 0);
        CHECK(portals[i].radius == Approx(2.5f * portals[i].scale));
        CHECK(portals[i].height == Approx(4.0f * portals[i].scale));
        CHECK(std::filesystem::is_regular_file(root / "levels" / portals[i].map / "worlds.wad"));
    }
    usize maps = 0;
    usize exits = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        CAPTURE(entry.path().filename().string());
        const auto decoded = decodeLegendsN64Portals(readFile(entry.path() / "ipr.rom"), types,
                                                     code, entry.path().filename().string());
        for (const auto& exit : decoded) {
            CHECK_FALSE(exit.map.empty());
        }
        exits += decoded.size();
        ++maps;
    }
    CHECK(maps == 34);
    CHECK(exits == 46);
}
} // namespace
