#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/n64/formats/LegendsN64Tower.h"

using namespace gdl;
using namespace gdl::formats;

TEST_CASE("N64 Tower rejects missing or unrelated overlays", "[formats][legends][tower]") {
    REQUIRE_THROWS_AS(decodeLegendsN64Tower({}), FormatError);
    const std::vector<u8> unrelated(44112);
    REQUIRE_THROWS_AS(decodeLegendsN64Tower(unrelated), FormatError);
}

TEST_CASE("Native N64 Tower creates its scripted residents and realm doors",
          "[formats][legends][tower][native]") {
    const auto file =
        test::dataDirectory().parent_path() / "assets/n64/extracted/_program/Tower.bin";
    if (!std::filesystem::exists(file)) {
        SKIP("Native N64 Legends overlay unavailable");
    }
    auto bytes = readFile(file);
    const auto scene = decodeLegendsN64Tower(bytes);
    REQUIRE(scene.sumner.models == "sum");
    REQUIRE(scene.sumner.animations == "wiz/anim");
    REQUIRE(scene.sumner.group == "WIZMOVES");
    REQUIRE(scene.sumner.sequence == "LKDN");
    REQUIRE(scene.sumner.position == Vec3(6, 0, -8));
    REQUIRE(scene.sumner.yaw == Catch::Approx(2.617994f));
    REQUIRE(scene.podium.models == "items/tower");
    REQUIRE(scene.podium.animations == "items/tower");
    REQUIRE(scene.podium.group == "PODIUM");
    REQUIRE(scene.podium.sequence == "REDY");
    REQUIRE(scene.podium.position == scene.sumner.position);
    REQUIRE(scene.podium.yaw == scene.sumner.yaw);
    const auto& body = scene.residentVolumes[0];
    REQUIRE(body.shape == LegendsN64TowerVolume::Shape::Cylinder);
    REQUIRE(body.centre == Vec3(6, 3, -8));
    REQUIRE(body.halfExtents == Vec3(1.5f, 3, 1.5f));
    REQUIRE(body.nativeCategory == 7);
    const auto& podium = scene.residentVolumes[1];
    REQUIRE(podium.shape == LegendsN64TowerVolume::Shape::Box);
    REQUIRE(podium.centre == Vec3(163.0f / 32.0f, 2.5f, -305.0f / 32.0f));
    REQUIRE(podium.halfExtents == Vec3(.5f, 2.5f, 20.0f / 32.0f));
    REQUIRE(podium.yaw == Catch::Approx(-2.617738f));
    REQUIRE(podium.nativeCategory == 14);
    REQUIRE(scene.staffModel == "SUM_STAFF");
    REQUIRE(scene.staffParent == "ARMR3");
    REQUIRE_FALSE(scene.staffVisibleInIdle);
    REQUIRE(scene.initiallyHiddenWorldObjects == std::vector<std::string>{"CATH_BEAM"});
    const auto& town = scene.doors[0];
    REQUIRE(town.openingSequence == "TWNA");
    REQUIRE(town.openSequence == "TWNO");
    REQUIRE(town.realmMask == 0x80);
    REQUIRE(town.realm == 7);
    REQUIRE(town.triggerPosition == Vec3(-41, 4, 25));
    REQUIRE(town.triggerRadius == 9);
    REQUIRE(town.requiredObelisks == 0x60);
    const auto& castle = scene.doors[1];
    REQUIRE(castle.openingSequence == "CASA");
    REQUIRE(castle.openSequence == "CASO");
    REQUIRE(castle.realmMask == 1);
    REQUIRE(castle.realm == 0);
    REQUIRE(castle.triggerPosition == Vec3(42, 2, -24));
    REQUIRE(castle.requiredObelisks == 7);
    const auto& ice = scene.doors[2];
    REQUIRE(ice.openingSequence == "ICEA");
    REQUIRE(ice.openSequence == "ICEO");
    REQUIRE(ice.realmMask == 0x40);
    REQUIRE(ice.realm == 6);
    REQUIRE(ice.triggerPosition == Vec3(37, 4, 22));
    REQUIRE(ice.requiredObelisks == 0x18);
    const auto& battle = scene.doors[3];
    REQUIRE(battle.openingSequence == "BTLA");
    REQUIRE(battle.openSequence == "BTLO");
    REQUIRE(battle.realmMask == 0x1000);
    REQUIRE(battle.realm == 12);
    REQUIRE(battle.triggerPosition == Vec3(0, -4, 45));
    REQUIRE(battle.requiredObelisks == 0x80);
    const auto nativeRoot = file.parent_path().parent_path();
    const auto program = readFile(nativeRoot / "_program/game.bin");
    for (const auto& door : scene.doors) {
        bool found = false;
        for (usize row = 0; row < 10; ++row) {
            const usize at = 0xe006c360U - 0xe0000400U + row * 24;
            if (readU32BE(program, at) == door.realm) {
                REQUIRE(readU32BE(program, at + 16) == door.requiredObelisks);
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
    const std::array<std::string_view, 13> runeNames{
        "RUNEC1", "RUNEC2", "RUNEC3", "RUNEM1", "RUNEM2", "RUNEM3", "RUNED1",
        "RUNED2", "RUNED3", "RUNEF1", "RUNEF2", "RUNEF3", "RUNEB1"};
    for (usize i = 0; i < scene.runestones.size(); ++i) {
        const auto& rune = scene.runestones[i];
        CAPTURE(i, rune.archive, rune.model);
        REQUIRE(rune.model == runeNames[i]);
        REQUIRE(rune.archive == "items/s/r" + std::to_string(i + 1));
        const auto models =
            decodeLegendsN64Models(readFile(nativeRoot / rune.archive / "objects.wad"));
        REQUIRE(std::ranges::any_of(models,
                                    [&](const auto& model) { return model.name == rune.model; }));
        const Vec3 position{rune.placement[3]};
        REQUIRE(position.y == Catch::Approx(.2f));
        REQUIRE(glm::length(Vec2{position.x - 1.5f, position.z - 14}) == Catch::Approx(5.3f));
        for (s32 axis = 0; axis < 3; ++axis) {
            REQUIRE(glm::length(Vec3{rune.placement[axis]}) == Catch::Approx(1.8f));
        }
    }
    const std::array<std::string_view, 4> shardNames{"SHARD_M", "SHARD_I", "SHARD_C", "SHARD_T"};
    const std::array<f32, 4> shardHeights{2.24f, 11.2f, 6.16f, 15.68f};
    for (usize i = 0; i < scene.shards.size(); ++i) {
        const auto& shard = scene.shards[i];
        CAPTURE(i, shard.archive, shard.model);
        REQUIRE(shard.model == shardNames[i]);
        const auto models =
            decodeLegendsN64Models(readFile(nativeRoot / shard.archive / "objects.wad"));
        REQUIRE(std::ranges::any_of(models,
                                    [&](const auto& model) { return model.name == shard.model; }));
        REQUIRE(shard.placement[3].x == 0);
        REQUIRE(shard.placement[3].y == Catch::Approx(shardHeights[i]));
        REQUIRE(shard.placement[3].z == -47);
        REQUIRE(shard.nativeDrawFlags == 0x10001a80U);
        REQUIRE(glm::length(Vec3{shard.placement[0]}) == Catch::Approx(2.8f));
    }
    REQUIRE(scene.shardPortal.group == "TRAP");
    REQUIRE(scene.shardPortal.sequence == "EXNR");
    REQUIRE(scene.shardPortal.position == Vec3{0, 1, -30});
    REQUIRE(scene.shardPortalScale == Catch::Approx(1.3f));
    // Fixed-address overlay consumers must not silently accept another build.
    bytes[0] ^= 1;
    REQUIRE_THROWS_AS(decodeLegendsN64Tower(bytes), FormatError);
}
