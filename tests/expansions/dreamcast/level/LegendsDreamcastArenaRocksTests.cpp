#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/level/LegendsDreamcastArenaRocks.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** B6's SAFEROCK definition. */
formats::ItemInfoRecord safeRock() {
    formats::ItemInfoRecord info;
    info.type = 10;
    info.subtype = 41;
    info.name = "SAFEROCK";
    info.collisionType = 1;
    info.radius = 2.3f;
    info.height = 5;
    info.hitPoints = 40;
    info.armor = 10;
    return info;
}

/** A placement of three standing segments. */
formats::ItemInstanceRecord placement() {
    formats::ItemInstanceRecord instance;
    instance.position = {4, 1, -9};
    instance.rotation.y = .5f;
    test::setU16LE(instance.params, 2, 3);
    return instance;
}

TEST_CASE("Dreamcast safe rocks stand their placed segments as arena cover",
          "[legends][dreamcast-arena-cover]") {
    std::string diagnostic = "stale";
    const auto rock = legendsDreamcastArenaRock(safeRock(), placement(), 0, &diagnostic);
    REQUIRE(rock);
    CHECK(diagnostic.empty());
    CHECK(rock->body.position == Vec3{4, 1, -9});
    CHECK(rock->body.yaw == .5f);
    CHECK(rock->body.radius == 2.3f);
    CHECK(rock->body.height == 5);
    CHECK(rock->body.tierHealth == 40);
    CHECK(rock->tier == 3);
    CHECK(rock->armor == 10);
    CHECK_FALSE(rock->dormant);
    auto hidden = placement();
    hidden.flags = 2;
    const auto dormant = legendsDreamcastArenaRock(safeRock(), hidden, 0);
    REQUIRE(dormant);
    CHECK(dormant->dormant);
    // The roster's last place is still a place.
    CHECK(legendsDreamcastArenaRock(safeRock(), placement(), kMostDreamcastArenaRocks - 1));

    SECTION("the placement's own positive subtype decides what it is") {
        auto barrel = safeRock();
        barrel.subtype = 43;
        auto overridden = placement();
        test::setU16LE(overridden.params, 0, 41);
        CHECK(legendsDreamcastArenaRock(barrel, overridden, 0));
        test::setU16LE(overridden.params, 0, 43);
        CHECK_FALSE(legendsDreamcastArenaRock(safeRock(), overridden, 0, &diagnostic));
        CHECK(diagnostic.empty());
    }
    SECTION("what is no safe rock is left to the ordinary fixtures without a report") {
        auto barrel = safeRock();
        barrel.subtype = 43;
        CHECK_FALSE(legendsDreamcastArenaRock(barrel, placement(), 0, &diagnostic));
        CHECK(diagnostic.empty());
        auto chest = safeRock();
        chest.type = 2;
        CHECK_FALSE(legendsDreamcastArenaRock(chest, placement(), 0, &diagnostic));
        CHECK(diagnostic.empty());
    }
}

TEST_CASE("Dreamcast safe rocks the arena cannot stand are reported",
          "[legends][dreamcast-arena-cover]") {
    constexpr auto kSegments = "Dreamcast arena rock has an unsupported segment count";
    constexpr auto kDurability = "Dreamcast arena rock has unsupported durability";
    constexpr auto kBody = "Dreamcast arena rock has an unsupported collision body";
    struct Case {
        const char* what;
        formats::ItemInfoRecord info;
        formats::ItemInstanceRecord instance;
        usize standing;
        std::string diagnostic;
    };
    std::vector<Case> cases;
    const auto add = [&](const char* what, auto change, std::string diagnostic,
                         usize standing = 0) {
        Case entry{what, safeRock(), placement(), standing, std::move(diagnostic)};
        change(entry.info, entry.instance);
        cases.push_back(std::move(entry));
    };
    add(
        "more segments than models",
        [](auto&, auto& instance) { test::setU16LE(instance.params, 2, 4); }, kSegments);
    add(
        "a negative segment count",
        [](auto&, auto& instance) { test::setU16LE(instance.params, 2, 0xffffU); }, kSegments);
    add("no health", [](auto& info, auto&) { info.hitPoints = 0; }, kDurability);
    add("implausible health", [](auto& info, auto&) { info.hitPoints = 10001; }, kDurability);
    add("a box body", [](auto& info, auto&) { info.collisionType = 3; }, kBody);
    add("no radius", [](auto& info, auto&) { info.radius = 0; }, kBody);
    add(
        "a nonfinite height",
        [](auto& info, auto&) { info.height = std::numeric_limits<f32>::quiet_NaN(); }, kBody);
    add("an offset body", [](auto& info, auto&) { info.collisionOffset = {0, 1, 0}; }, kBody);
    add(
        "a rock past the boss's roster", [](auto&, auto&) {},
        "Dreamcast arena has more safe rocks than the boss's 16", kMostDreamcastArenaRocks);
    for (const auto& entry : cases) {
        CAPTURE(entry.what);
        std::string diagnostic;
        CHECK_FALSE(
            legendsDreamcastArenaRock(entry.info, entry.instance, entry.standing, &diagnostic));
        CHECK(diagnostic == entry.diagnostic);
        CHECK_FALSE(legendsDreamcastArenaRock(entry.info, entry.instance, entry.standing));
    }
}

TEST_CASE("Dreamcast safe rocks the arena cannot stand are reported once and never placed",
          "[legends][dreamcast-scenery][dreamcast-arena-cover][assets]") {
    const auto native = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    const auto source = native / "levels/levelb6";
    if (!std::filesystem::is_regular_file(source / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    // A copy of B6 whose six rocks share a definition without durability.
    const auto root = test::scratchDirectory("dreamcast-unstandable-rocks");
    const auto copy = [&](const std::string& relative) {
        if (!std::filesystem::exists(native / relative) ||
            std::filesystem::exists(root / relative)) {
            return;
        }
        std::filesystem::create_directories((root / relative).parent_path());
        std::filesystem::copy(native / relative, root / relative,
                              std::filesystem::copy_options::recursive);
    };
    copy("levels/levelb6");
    const auto metadata = formats::dreamcastLegendsWorldDataFile("levelb6");
    copy(metadata);
    const auto scope =
        formats::dreamcastLegendsFixtureScope(readFile(native / metadata), "levelb6");
    REQUIRE(scope);
    for (const auto& archive : scope->archives) {
        copy(archive);
    }
    auto bytes = readFile(source / "worlds.rom");
    const auto scene = formats::LegendsDreamcastScene::parse(bytes);
    const auto definition = static_cast<usize>(scene.itemInstances.at(27).info);
    REQUIRE(scene.itemInfos.at(definition).subtype == 41);
    REQUIRE(std::ranges::count(scene.itemInstances, scene.itemInstances.at(27).info,
                               &formats::ItemInstanceRecord::info) == 6);
    constexpr usize kHitPoints = 68; // within the 80-byte native item definition
    test::setU16LE(
        bytes, readU32LE(bytes, 76) + definition * formats::ItemInfoRecord::kSize + kHitPoints, 0);
    writeFile(root / "levels/levelb6/worlds.rom", bytes);

    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(source / "objects.rom"));
    test::FakeRenderDevice device;
    const auto adapter =
        LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, root / "levels/levelb6", map);
    REQUIRE(adapter);
    CHECK(adapter->arenaTargets().empty());
    CHECK(adapter->arenaObstacles().empty());
    CHECK(std::ranges::count(adapter->warnings(),
                             "Dreamcast arena rock has unsupported durability: SAFEROCK") == 1);
    for (usize index = 0; index < adapter->fixtureCount(); ++index) {
        const auto figure = adapter->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase +
                                                   static_cast<s32>(index));
        CAPTURE(index);
        CHECK_FALSE((figure && figure->name.starts_with("SAFEROCK")));
    }
}
} // namespace
