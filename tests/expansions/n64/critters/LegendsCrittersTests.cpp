#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/critters/LegendsCritters.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/EnemyMind.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
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
struct Native {
    std::vector<u8> ipr = std::vector<u8>(108);
    std::vector<u8> pgen = std::vector<u8>(60);
    std::vector<u8> program = std::vector<u8>(0x6d000);
    static constexpr usize kRealm = 0xe006c360U - 0xe0000400U;
    static constexpr usize kLevel = 0xe006c468U - 0xe0000400U;
    Native() {
        put32(ipr, 8, 92);
        put16(ipr, 40, 1);
        put16(ipr, 92, 40);
        put16(ipr, 94, 80);
        put16(ipr, 96, static_cast<u16>(-120));
        put16(ipr, 98, 16);
        put16(ipr, 100, 0x0508);
        ipr[102] = 1;
        ipr[103] = 19;
        ipr[105] = 3;
        ipr[106] = 2;
        put32(pgen, 0, 1);
        put16(pgen, 4, 0x0508);
        put32(pgen, 8, 0x01000000);
        pgen[13] = 6;
        pgen[14] = 1;
        put32(pgen, 32, 5);
        real(pgen, 48, 16);
        real(pgen, 52, 10);
        put32(program, kRealm, 0);
        put32(program, kRealm + 8, 0xe0000400U + 100);
        program[kRealm + 12] = 5;
        program[kRealm + 13] = 4;
        program[kRealm + 14] = 3;
        program[kRealm + 15] = 'C';
        put32(program, kRealm + 24, 16);
        put32(program, kLevel, 0);
        put32(program, kLevel + 12, 0xe0000400U + 120);
        program[kLevel + 16] = program[kLevel + 17] = program[kLevel + 18] = 0x22;
        put32(program, kLevel + 40, 16);
        const std::string stem = "castle";
        std::ranges::copy(stem, program.begin() + 100);
        program[120] = '1';
    }
    LegendsActorRoster
    decode(std::span<const formats::LegendsMapCollisionTriangle> floor = {}) const {
        return decodeLegendsN64Actors(ipr, pgen, program, "CASTLE1", floor);
    }
};
} // namespace

TEST_CASE("Native one-shot actors retain coordinates and separate pending from live facing",
          "[legends][legends-actors][formats]") {
    const Native native;
    const auto result = native.decode();
    REQUIRE(result.warnings.empty());
    REQUIRE(result.actors.size() == 1);
    const auto& actor = result.actors.front();
    CHECK(actor.nativeIndex == 0);
    CHECK(actor.spawn.kind == kGolemEnemyKind);
    CHECK(actor.spawn.position == Vec3{10, 20, -30});
    CHECK(actor.spawn.direction == Vec3{0, 0, 1});
    CHECK(actor.idleYaw == .5f);
    CHECK(actor.yaw == 0);
    CHECK(actor.minimumPlayers == 2);
    CHECK(actor.activationRadius == 16);
    CHECK(actor.activationHeight == 10);
    CHECK(actor.costumeRealm == 'A');
    CHECK(actor.floorObject == -1);
    CHECK(actor.spawn.placed);
    CHECK(actor.spawn.clearance == 0);
    const std::array floor{formats::LegendsMapCollisionTriangle{
        .vertices = {Vec3{-100, 19, -100}, Vec3{-100, 19, 100}, Vec3{100, 19, -100}},
        .owner = 19,
        .floor = true}};
    const auto projected = native.decode(floor).actors.front();
    CHECK(projected.spawn.position.x == 10);
    CHECK(projected.spawn.position.z == -30);
    CHECK(projected.spawn.position.y == Approx(19.2f));
    CHECK(projected.floorObject == 19);
}

TEST_CASE(
    "Native ordinary actors translate the actual large roster and strategy rather than raw IDs",
    "[legends][legends-actors][formats]") {
    Native native;
    native.pgen[13] = 2;
    native.ipr[103] = 15;
    auto actor = native.decode().actors.front();
    CHECK(actor.spawn.kind == 5);
    CHECK(actor.spawn.algorithm == kChaseWay);
    CHECK(actor.yaw == actor.idleYaw);
    CHECK(actor.spawn.direction.x == Approx(std::sin(.5f)));
    CHECK(actor.spawn.direction.z == Approx(std::cos(.5f)));
    native.ipr[102] = native.pgen[14] = 4;
    native.ipr[103] = 23;
    actor = native.decode().actors.front();
    CHECK(actor.spawn.kind == 4); // realm large != medium
    CHECK(actor.spawn.tier == 4);
    CHECK(actor.spawn.algorithm == kThrowWay);
    native.program[Native::kLevel + 17] = 12;
    CHECK(native.decode().actors.front().spawn.kind == 16); // level override
    native.ipr[102] = native.pgen[14] = 5;
    native.ipr[103] = 17;
    CHECK(native.decode().actors.front().spawn.algorithm == kSkirmishBombWay);
    native.ipr[102] = native.pgen[14] = 6;
    native.ipr[103] = 18;
    CHECK(native.decode().actors.front().spawn.algorithm == kSuicideWay);
}

TEST_CASE("Native It placements become the It mechanic not generals",
          "[legends][legends-actors][formats]") {
    Native native;
    native.pgen[13] = 9;
    native.ipr[106] = 3;
    real(native.pgen, 48, 40);
    const auto actor = native.decode().actors.front();
    CHECK(actor.spawn.kind == kItKind);
    CHECK(actor.spawn.algorithm == kLurkWay);
    CHECK(actor.minimumPlayers == 3);
    CHECK(actor.activationRadius == 40);
    CHECK(actor.yaw == 0);
}

TEST_CASE("Actor proximity uses strict native vertical separation and real living player positions",
          "[legends][legends-actors]") {
    const auto actor = Native{}.decode().actors.front();
    std::array players{EnemyView{.player = 0, .position = actor.spawn.position + Vec3{16, 0, 0}}};
    CHECK(legendsActorReady(actor, players));
    CHECK_FALSE(legendsActorReady(actor, players, true));
    CHECK_FALSE(legendsActorReady(actor, {}));
    players[0].position.y += 10;
    CHECK_FALSE(legendsActorReady(actor, players));
    players[0].position.y -= .001f;
    CHECK(legendsActorReady(actor, players));
    players[0].hidden = players[0].invisible = true;
    players[0].decoy = Vec3{999};
    CHECK(legendsActorReady(actor, players));
    players[0].position.x += .01f;
    CHECK_FALSE(legendsActorReady(actor, players));
    players[0].position = actor.spawn.position;
    players[0].player = -1;
    CHECK_FALSE(legendsActorReady(actor, players));
    players[0].player = 0;
    players[0].position.y = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(legendsActorReady(actor, players));
    auto sphere = actor;
    sphere.activationHeight = 0;
    players[0].position = actor.spawn.position + Vec3{0, 17, 0};
    CHECK_FALSE(legendsActorReady(sphere, players));
}

TEST_CASE("Native actor decoder rejects truncation and diagnoses unsupported placements",
          "[legends][legends-actors][formats]") {
    Native native;
    SECTION("truncated IPR") {
        native.ipr.pop_back();
        CHECK_THROWS_AS(native.decode(), FormatError);
    }
    SECTION("truncated templates") {
        native.pgen.pop_back();
        CHECK_THROWS_AS(native.decode(), FormatError);
    }
    SECTION("truncated native roster") {
        native.program.resize(20);
        CHECK_THROWS_AS(native.decode(), FormatError);
    }
    SECTION("unknown template") {
        put16(native.ipr, 100, 0xffff);
        CHECK_THROWS_AS(native.decode(), FormatError);
    }
    SECTION("invalid range") {
        real(native.pgen, 48, -1);
        CHECK_THROWS_AS(native.decode(), FormatError);
    }
    SECTION("non-finite range") {
        real(native.pgen, 52, std::numeric_limits<f32>::infinity());
        CHECK_THROWS_AS(native.decode(), FormatError);
    }
    SECTION("trigger-dependent actor") {
        native.ipr[105] |= 8;
        CHECK(native.decode().actors.empty());
        CHECK(native.decode().warnings.size() == 1);
    }
    SECTION("unknown class") {
        native.pgen[13] = 11;
        CHECK(native.decode().actors.empty());
        CHECK(native.decode().warnings.size() == 1);
    }
    SECTION("mixed native roster") {
        native.pgen[13] = 2;
        native.program[Native::kLevel + 16] = 33;
        CHECK(native.decode().actors.empty());
        CHECK(native.decode().warnings.size() == 1);
    }
    SECTION("ordinary repeating generator") {
        native.ipr[105] = 1;
        CHECK(native.decode().actors.empty());
        CHECK(native.decode().warnings.empty());
    }
}

TEST_CASE("All native N64 placed enemies and golems have audited descriptors",
          "[legends][legends-actors][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(root / "_program/game.bin")) {
        SKIP("native N64 Legends extraction is not installed");
    }
    const auto program = readFile(root / "_program/game.bin");
    const auto pgen = readFile(root / "items/generic/pgen.rom");
    usize maps = 0;
    usize ordinary = 0;
    usize golems = 0;
    usize its = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        const auto& path = entry.path();
        if (!std::filesystem::exists(path / "worlds.wad")) {
            continue;
        }
        INFO(path.filename().string());
        ++maps;
        const auto ipr = readFile(path / "ipr.rom");
        const auto result =
            decodeLegendsN64Actors(ipr, pgen, program, path.filename().string(), {});
        for (const auto& warning : result.warnings) {
            INFO(warning);
            CHECK(false);
        }
        usize expected = 0;
        const usize start = readU32BE(ipr, 8);
        for (usize i = 0; i < readU16BE(ipr, 40); ++i) {
            expected += (ipr[start + i * 16 + 13] & 2U) != 0 ? 1 : 0;
        }
        REQUIRE(result.actors.size() == expected);
        for (const auto& actor : result.actors) {
            const usize at = start + static_cast<usize>(actor.nativeIndex) * 16;
            CHECK(actor.spawn.position == Vec3{std::bit_cast<s16>(readU16BE(ipr, at)),
                                               std::bit_cast<s16>(readU16BE(ipr, at + 2)),
                                               std::bit_cast<s16>(readU16BE(ipr, at + 4))} /
                                              4.0f);
            if (actor.spawn.kind == kGolemEnemyKind) {
                ++golems;
                CHECK(actor.activationRadius == 16);
            } else if (actor.spawn.kind == kItKind) {
                ++its;
                CHECK(actor.minimumPlayers == 3);
            } else {
                ++ordinary;
            }
            // PGEn+0x30 is template-specific. The native proximity call
            // e0066b28 forwards it unchanged: ranged/suicide ETR reaches
            // twenty vertically, EGR fifteen, ordinary bodies/golems/It ten.
            const u16 key = readU16BE(ipr, at + 8);
            const bool variant = actor.spawn.kind < kSwarmKindCount && actor.spawn.tier >= 4;
            CHECK(actor.activationHeight == (variant ? key == 0x0503 ? 20 : 15 : 10));
            if (variant) {
                CHECK(actor.activationRadius == 60);
            }
        }
    }
    CHECK(maps == 34);
    CHECK(ordinary == 677);
    CHECK(golems == 61);
    CHECK(its == 6);
}
