#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Error.h"

#include "TestSupport.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBosses.h"
#include "game/level/BossKinds.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::vector<u8> syntheticWorldData() {
    constexpr usize kRealm = 8;
    constexpr usize kLevel = kRealm + 56;
    constexpr usize kDirectory = kLevel + 144;
    std::vector<u8> bytes(kDirectory + 24);
    test::setU32LE(bytes, 0, kDirectory);
    test::setU32LE(bytes, 4, 2);
    test::setU32LE(bytes, kRealm, 2);
    test::setText(bytes, kLevel + 8, 4, "B6");
    test::setU32LE(bytes, kLevel + 52, 26);
    test::setU32LE(bytes, kDirectory, 0x57524c44U);
    test::setU32LE(bytes, kDirectory + 4, kRealm);
    test::setU32LE(bytes, kDirectory + 8, 1);
    test::setU32LE(bytes, kDirectory + 12, 0x4c45564cU);
    test::setU32LE(bytes, kDirectory + 16, kLevel);
    test::setU32LE(bytes, kDirectory + 20, 1);
    return bytes;
}

std::vector<u8> syntheticWorld() {
    // WorldFile's native header uses offsets even for empty tables. The only
    // record this fixture contains is the actual type 6 boss locator.
    std::vector<u8> bytes(96 + 28);
    test::setU32LE(bytes, 88, 1);
    test::setU32LE(bytes, 92, 96);
    test::setU16LE(bytes, 96, static_cast<u16>(LocatorKind::Boss));
    test::setF32LE(bytes, 100, 12);
    test::setF32LE(bytes, 104, 8);
    test::setF32LE(bytes, 108, -23);
    test::setF32LE(bytes, 116, .4f);
    return bytes;
}

TEST_CASE("Dreamcast boss admission uses native level identity and its authored locator",
          "[legends][dreamcast-boss-completion][dreamcast-bosses]") {
    auto metadata = syntheticWorldData();
    auto world = syntheticWorld();
    const auto boss = legendsDreamcastBoss(metadata, world, "LeVeLb6");
    REQUIRE(boss);
    CHECK(boss->map == "levelb6");
    CHECK(boss->nativeKind == 26);
    CHECK(boss->nativeRealm == 2);
    CHECK(boss->archive == "dragon");
    CHECK(boss->combat.kind == boss_type::kDragon);
    CHECK(boss->combat.position == Vec3{12, 8, -23});
    CHECK(boss->combat.yaw == .4f);
    CHECK(boss->combat.counterpartStage == "B6");

    SECTION("ordinary and absent optional campaigns need no native boss resources") {
        CHECK_FALSE(legendsDreamcastBoss({}, {}, "levelb1"));
        CHECK_FALSE(legendsDreamcastBoss({}, {}, "levelt1"));
        CHECK_FALSE(legendsDreamcastBoss({}, {}, "mount6"));
        CHECK_FALSE(legendsDreamcastBoss({}, {}, "../levelb6"));
        CHECK_FALSE(legendsDreamcastBoss({}, {}, "levelg5"));
        CHECK_FALSE(legendsDreamcastBoss({}, {}, "leveli5"));
    }
    SECTION("an arena filename cannot turn an ordinary stage into a boss") {
        test::setU32LE(metadata, 8 + 56 + 52, 0xffffffffU);
        CHECK_FALSE(legendsDreamcastBoss(metadata, world, "levelb6"));
    }
    SECTION("missing boss marker is not replaced by the player start") {
        test::setU16LE(world, 96, static_cast<u16>(LocatorKind::Start));
        CHECK_FALSE(legendsDreamcastBoss(metadata, world, "levelb6"));
    }
    SECTION("malformed metadata and nonfinite positions are rejected") {
        test::setU32LE(metadata, 0, 0xffffffffU);
        CHECK_THROWS_AS(legendsDreamcastBoss(metadata, world, "levelb6"), FormatError);
        metadata = syntheticWorldData();
        test::setU32LE(metadata, 8 + 56 + 52, 33);
        CHECK_THROWS_AS(legendsDreamcastBoss(metadata, world, "levelb6"), FormatError);
        metadata = syntheticWorldData();
        test::setF32LE(world, 104, std::numeric_limits<f32>::quiet_NaN());
        CHECK_THROWS_AS(legendsDreamcastBoss(metadata, world, "levelb6"), FormatError);
    }
    SECTION("an arena's realm or kind must be its campaign's own") {
        constexpr auto kIdentity = "Dreamcast boss: arena identity disagrees with native campaign";
        test::setU32LE(metadata, 8, 3); // B6 belongs to the mountain, realm 2
        CHECK_THROWS_WITH(legendsDreamcastBoss(metadata, world, "levelb6"), kIdentity);
        metadata = syntheticWorldData();
        test::setU32LE(metadata, 8 + 56 + 52, 27); // the Chimera's native kind
        CHECK_THROWS_WITH(legendsDreamcastBoss(metadata, world, "levelb6"), kIdentity);
    }
    SECTION("an arena has one boss locator") {
        constexpr usize kLocator = 28;
        world.resize(96 + 2 * kLocator);
        test::setU32LE(world, 88, 2);
        test::setU16LE(world, 96 + kLocator, static_cast<u16>(LocatorKind::Boss));
        CHECK_THROWS_WITH(legendsDreamcastBoss(metadata, world, "levelb6"),
                          "Dreamcast boss: multiple boss locators");
    }
    SECTION("the combat adapter stands a boss upright") {
        constexpr auto kTilted =
            "Dreamcast boss: tilted boss locator is not supported by the combat adapter";
        test::setF32LE(world, 112, .05f); // pitch
        CHECK_THROWS_WITH(legendsDreamcastBoss(metadata, world, "levelb6"), kTilted);
        world = syntheticWorld();
        test::setF32LE(world, 120, -.05f); // roll
        CHECK_THROWS_WITH(legendsDreamcastBoss(metadata, world, "levelb6"), kTilted);
    }
}
} // namespace
