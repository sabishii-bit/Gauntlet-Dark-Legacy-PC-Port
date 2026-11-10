#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
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
    const auto admission = legendsDreamcastArenaRock(safeRock(), placement(), 0);
    REQUIRE(admission.rock);
    CHECK(admission.diagnostic.empty());
    CHECK_FALSE(admission.ordinary);
    const auto& rock = *admission.rock;
    CHECK(rock.body.position == Vec3{4, 1, -9});
    CHECK(rock.body.yaw == .5f);
    CHECK(rock.body.radius == 2.3f);
    CHECK(rock.body.height == 5);
    CHECK(rock.body.tierHealth == 40);
    CHECK(rock.tier == 3);
    CHECK(rock.armor == 10);
    CHECK_FALSE(rock.dormant);
    auto hidden = placement();
    hidden.flags = 2;
    const auto dormant = legendsDreamcastArenaRock(safeRock(), hidden, 0).rock;
    REQUIRE(dormant);
    CHECK(dormant->dormant);
    // The roster's last place is still a place.
    CHECK(legendsDreamcastArenaRock(safeRock(), placement(), kMostDreamcastArenaRocks - 1).rock);

    SECTION("the placement's own positive subtype decides what it is") {
        auto barrel = safeRock();
        barrel.subtype = 43;
        auto overridden = placement();
        test::setU16LE(overridden.params, 0, 41);
        CHECK(legendsDreamcastArenaRock(barrel, overridden, 0).rock);
        test::setU16LE(overridden.params, 0, 43);
        const auto other = legendsDreamcastArenaRock(safeRock(), overridden, 0);
        CHECK_FALSE(other.rock);
        CHECK(other.ordinary);
        CHECK(other.diagnostic.empty());
    }
    SECTION("what is no safe rock is left to the ordinary fixtures without a report") {
        auto barrel = safeRock();
        barrel.subtype = 43;
        auto chest = safeRock();
        chest.type = 2;
        for (const auto& info : {barrel, chest}) {
            CAPTURE(info.type, info.subtype);
            const auto other = legendsDreamcastArenaRock(info, placement(), 0);
            CHECK_FALSE(other.rock);
            CHECK(other.ordinary);
            CHECK(other.diagnostic.empty());
        }
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
        std::string diagnostic;
    };
    std::vector<Case> cases;
    const auto add = [&](const char* what, auto change, std::string diagnostic) {
        Case entry{what, safeRock(), placement(), std::move(diagnostic)};
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
    for (const auto& entry : cases) {
        CAPTURE(entry.what);
        // Reported, and never left to the ordinary fixtures as scenery.
        const auto admission = legendsDreamcastArenaRock(entry.info, entry.instance, 0);
        CHECK_FALSE(admission.rock);
        CHECK_FALSE(admission.ordinary);
        CHECK(admission.diagnostic == entry.diagnostic);
    }
}

TEST_CASE("Dreamcast safe rocks past the boss's roster are reported and stay ordinary items",
          "[legends][dreamcast-arena-cover]") {
    constexpr auto kRoster = "Dreamcast arena has more safe rocks than the boss's 16";
    const auto past = legendsDreamcastArenaRock(safeRock(), placement(), kMostDreamcastArenaRocks);
    CHECK_FALSE(past.rock);
    CHECK(past.ordinary);
    CHECK(past.diagnostic == kRoster);
    // Outside the roster a rock's own fields no longer matter: the arena never stands it.
    auto spent = safeRock();
    spent.hitPoints = 0;
    const auto unsupported =
        legendsDreamcastArenaRock(spent, placement(), kMostDreamcastArenaRocks + 3);
    CHECK_FALSE(unsupported.rock);
    CHECK(unsupported.ordinary);
    CHECK(unsupported.diagnostic == kRoster);
}

/** A scratch copy of the extracted Dreamcast B6 arena, with the metadata and archives it reads,
 * whose worlds.rom a case edits before loading it. */
struct ArenaCopy {
    std::filesystem::path native =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    std::filesystem::path source = native / "levels/levelb6";
    std::filesystem::path root;
    std::vector<u8> worlds;
    formats::LegendsDreamcastScene scene;

    explicit ArenaCopy(std::string_view name) {
        if (!std::filesystem::is_regular_file(source / "worlds.rom")) {
            SKIP("Optional Dreamcast extraction is not installed");
        }
        root = test::scratchDirectory(name);
        copy("levels/levelb6");
        const auto metadata = formats::dreamcastLegendsWorldDataFile("levelb6");
        copy(metadata);
        const auto scope =
            formats::dreamcastLegendsFixtureScope(readFile(native / metadata), "levelb6");
        REQUIRE(scope);
        for (const auto& archive : scope->archives) {
            copy(archive);
        }
        worlds = readFile(source / "worlds.rom");
        scene = formats::LegendsDreamcastScene::parse(worlds);
    }

    /** B6's safe rock definition, which its placements 27 to 32 share. */
    usize rockDefinition() const {
        const auto definition = static_cast<usize>(scene.itemInstances.at(27).info);
        REQUIRE(scene.itemInfos.at(definition).subtype == 41);
        REQUIRE(std::ranges::count(scene.itemInstances, scene.itemInstances.at(27).info,
                                   &formats::ItemInstanceRecord::info) == 6);
        return definition;
    }

    /** Writes the edited worlds.rom into the copy and loads it as a one-player arena. */
    std::unique_ptr<LegendsSceneAdapter> load(test::FakeRenderDevice& device) const {
        writeFile(root / "levels/levelb6/worlds.rom", worlds);
        auto map = formats::LegendsMap::parseDreamcast(worlds, readFile(source / "objects.rom"));
        return LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast,
                                         root / "levels/levelb6", map);
    }

private:
    void copy(const std::string& relative) const {
        if (!std::filesystem::exists(native / relative) ||
            std::filesystem::exists(root / relative)) {
            return;
        }
        std::filesystem::create_directories((root / relative).parent_path());
        std::filesystem::copy(native / relative, root / relative,
                              std::filesystem::copy_options::recursive);
    }
};

TEST_CASE("Dreamcast safe rocks the arena cannot stand are reported once and never placed",
          "[legends][dreamcast-scenery][dreamcast-arena-cover][assets]") {
    ArenaCopy arena("dreamcast-unstandable-rocks");
    // B6's six rocks share a definition, here without durability.
    const usize definition = arena.rockDefinition();
    constexpr usize kHitPoints = 68; // within the 80-byte native item definition
    test::setU16LE(
        arena.worlds,
        readU32LE(arena.worlds, 76) + definition * formats::ItemInfoRecord::kSize + kHitPoints, 0);
    test::FakeRenderDevice device;
    const auto adapter = arena.load(device);
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

TEST_CASE("a Dreamcast safe rock past the boss's roster stands as an ordinary fixture",
          "[legends][dreamcast-scenery][dreamcast-arena-cover][assets]") {
    ArenaCopy arena("dreamcast-roster-rocks");
    // Eleven of B6's twelve flame vents become its safe rock ahead of its own six, so its own
    // last rock, placement 32, is the seventeenth in item order.
    const usize definition = arena.rockDefinition();
    const auto& rock = arena.scene.itemInstances.at(27);
    constexpr usize kParams = 48; // within the 60-byte native placement
    const usize placements = readU32LE(arena.worlds, 84);
    usize converted = 0;
    for (usize index = 0; index < arena.scene.itemInstances.size() && converted < 11; ++index) {
        const auto& vent = arena.scene.itemInstances[index];
        if (arena.scene.itemInfos.at(static_cast<usize>(vent.info)).name != "FLAMEV") {
            continue;
        }
        REQUIRE(index < 27);
        const usize at = placements + index * formats::ItemInstanceRecord::kSize;
        test::setU16LE(arena.worlds, at, static_cast<u16>(definition));
        for (usize param = 0; param < rock.params.size(); ++param) {
            arena.worlds[at + kParams + param] = rock.params[param];
        }
        ++converted;
    }
    REQUIRE(converted == 11);
    test::FakeRenderDevice device;
    const auto adapter = arena.load(device);
    REQUIRE(adapter);
    const auto targets = adapter->arenaTargets();
    CHECK(targets.size() == kMostDreamcastArenaRocks);
    CHECK(std::ranges::count(adapter->warnings(),
                             "Dreamcast arena has more safe rocks than the boss's 16: SAFEROCK") ==
          1);
    const Vec3 last = arena.scene.itemInstances.at(32).position;
    const auto atLast = [&](const Vec3& position) {
        return std::abs(position.x - last.x) < .01f && std::abs(position.z - last.z) < .01f;
    };
    for (const auto& target : targets) {
        CHECK_FALSE(atLast(Vec3{target.placement[3]}));
    }
    usize scenery = 0;
    for (usize index = 0; index < adapter->fixtureCount(); ++index) {
        const auto figure = adapter->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase +
                                                   static_cast<s32>(index));
        if (figure && figure->visible && figure->name == "SAFEROCK3" && atLast(figure->position)) {
            ++scenery;
        }
    }
    CHECK(scenery == 1);
}
} // namespace
