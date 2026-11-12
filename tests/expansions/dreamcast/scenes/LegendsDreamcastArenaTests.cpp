#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/level/LegendsDreamcastArenaRocks.h"
#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/level/MissileStop.h"

namespace {
using namespace gdl;
using namespace gdl::game;

usize placedMeshVertices(test::FakeRenderDevice& device, const LegendsSceneAdapter& adapter,
                         const Mesh& mesh, const Mat4& placement) {
    std::vector<Vec3> expected;
    for (const auto& part : mesh.parts) {
        for (const auto index : part.indices) {
            expected.emplace_back(placement * Vec4{mesh.vertices[index].position, 1});
        }
    }
    device.draws.clear();
    adapter.draw(device, Mat4{1}, CameraFrame{}, TreeModel::Pass::All);
    usize actual = 0;
    // TreeModel submits world-space vertices with the clip matrix, so draw.transform
    // alone does not identify the item. Check its actual placed geometry instead.
    for (const auto& draw : device.draws) {
        for (const auto& vertex : draw.vertices) {
            const Vec3 point{draw.transform * Vec4{vertex.position, 1}};
            if (std::ranges::any_of(expected, [&](const auto& value) {
                    return glm::distance(point, value) < .0001f;
                })) {
                ++actual;
            }
        }
    }
    return actual;
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

TEST_CASE("Dreamcast Chimera arena stands no cover though its archive holds the rock",
          "[legends][dreamcast-scenery][dreamcast-arena-cover][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levela5";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    const auto records = formats::LegendsDreamcastScene::parse(bytes);
    // CollectSafeRocks 8c0a9ce0 takes only type 10 items of subtype 41. A5 defines no type 10
    // item at all, so CritterProcessSafeRocks 8c08aa00 leaves the roster empty (8c08aa4a).
    CHECK(
        std::ranges::none_of(records.itemInfos, [](const auto& info) { return info.type == 10; }));
    auto art = loadLegendsDreamcastArchive(directory.parent_path().parent_path() / "items/levela5");
    REQUIRE(art);
    REQUIRE(art->models.find("SAFEROCK1L1"));
    const auto objectBytes = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(bytes, objectBytes);
    map.textures =
        formats::decodeLegendsDreamcastTextures(objectBytes, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    // A full party shows every placement, and CollectSafeRocks never reads the party flag.
    constexpr s32 kFullParty = 4;
    auto adapter =
        LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, kFullParty);
    REQUIRE(adapter);
    CHECK(adapter->arenaTargets().empty());
    CHECK(adapter->arenaDamageTargets().empty());
    CHECK(adapter->arenaObstacles().empty());
    CHECK(adapter->arenaAnchors().empty());
    CHECK(adapter->arenaMissileStops().empty());
    // The level's archive keeps the tier-one rock, but no fixture places or draws it.
    for (usize index = 0; index < adapter->fixtureCount(); ++index) {
        const auto figure = adapter->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase +
                                                   static_cast<s32>(index));
        CAPTURE(index);
        CHECK_FALSE((figure && figure->name.starts_with("SAFEROCK")));
    }
    const auto root = test::assetOrSkip("PLAYERS").parent_path();
    LegendsLevel level{device, map, std::move(adapter), directory, root};
    CHECK(level.arenaTargets().empty());
    CHECK(level.arenaAnchors().empty());
    CHECK(level.arenaMissileStops().empty());
    CHECK(level.targets().empty());
    CHECK(level.obstacles().empty());
    CHECK_FALSE(level.strike(kImportedArenaBase, 1000, 0, 0));
}

TEST_CASE("Dreamcast Dragon arena safe rocks block and change native health tiers",
          "[legends][dreamcast-scenery][dreamcast-arena-cover][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb6";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    const auto scene = formats::LegendsDreamcastScene::parse(bytes);
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(adapter);
    const auto anchors = adapter->arenaTargets();
    const auto targets = adapter->arenaDamageTargets();
    const auto bodies = adapter->arenaObstacles();
    const auto stops = adapter->arenaMissileStops();
    REQUIRE(anchors.size() == 6);
    REQUIRE(targets.size() == 6);
    REQUIRE(bodies.size() == 6);
    REQUIRE(stops.size() == 6);
    for (usize i = 0; i < targets.size(); ++i) {
        const auto& item = scene.itemInstances.at(27 + i);
        const auto& info = scene.itemInfos.at(item.info);
        REQUIRE(info.type == 10);
        REQUIRE(info.subtype == 41);
        CHECK(info.name == "SAFEROCK");
        CHECK(info.hitPoints == 40);
        CHECK(info.armor == 10);
        CHECK(item.params[2] == 3);
        CHECK(anchors[i].active);
        CHECK(targets[i].valid());
        CHECK(targets[i].id == kImportedArenaBase + static_cast<s32>(i));
        CHECK(targets[i].radius == info.radius);
        CHECK(targets[i].height == info.height);
        CHECK(bodies[i].solid);
        CHECK(bodies[i].cylinderRadius == info.radius);
        CHECK(stops[i].rockHealth == 120);
        CHECK(stops[i].rockArmor == 10);
        const Vec3 centre = bodies[i].centre + Vec3{0, 1, 0};
        CHECK(adapter->arenaBlocksSegment(centre - Vec3{4, 0, 0}, centre + Vec3{4, 0, 0}, .1f));
    }
    auto art = loadLegendsDreamcastArchive(directory.parent_path().parent_path() / "items/levelb6");
    REQUIRE(art);
    const auto checkTierDraw = [&](s32 tier) {
        const auto model = art->models.find("SAFEROCK" + std::to_string(tier) + "L1");
        REQUIRE(model);
        usize expected = 0;
        for (const auto& part : art->models.mesh(*model).parts) {
            expected += part.indices.size();
        }
        const auto placement =
            legendsDreamcastItemPlacement(bodies[0].centre, scene.itemInstances.at(27).rotation);
        const usize actual =
            placedMeshVertices(device, *adapter, art->models.mesh(*model), placement);
        CHECK(expected > 0);
        CHECK(actual == expected);
    };
    checkTierDraw(3);
    CHECK_FALSE(adapter->strikeArenaRock(6, 100));
    CHECK_FALSE(adapter->strikeArenaRock(0, std::numeric_limits<f32>::infinity()));
    CHECK(adapter->strikeArenaRock(0, 10)); // native armor still admits minimum one
    CHECK(adapter->arenaMissileStops()[0].rockHealth == 119);
    CHECK(adapter->strikeArenaRock(0, 49.75f)); // truncates 39.75 after armor
    CHECK(adapter->arenaMissileStops()[0].rockHealth == 80);
    checkTierDraw(2);
    CHECK(adapter->strikeArenaRock(0, 50));
    CHECK(adapter->arenaMissileStops()[0].rockHealth == 40);
    checkTierDraw(1);
    CHECK(adapter->strikeArenaRock(0, 1000));
    CHECK_FALSE(adapter->arenaTargets()[0].active);
    CHECK(adapter->arenaDamageTargets().size() == 5);
    CHECK(adapter->arenaObstacles().size() == 5);
    CHECK(adapter->arenaMissileStops().size() == 5);
    const auto burst = art->trees.find("SAFEREXP");
    REQUIRE(burst);
    const auto& burstSequence = art->trees.tree(*burst).sequences.at(0);
    REQUIRE(burstSequence.frames > 0);
    // Body removal is immediate. The native visual burst finishes before
    // settling on the static tier-zero rubble instead of looping forever.
    const f32 burstSeconds =
        static_cast<f32>(burstSequence.frames) *
        static_cast<f32>(burstSequence.frameRate > 0 ? burstSequence.frameRate : 30) / 900.f;
    adapter->update(burstSeconds + 1);
    checkTierDraw(0);
    CHECK_FALSE(adapter->strikeArenaRock(0, 100));
    const Vec3 centre = bodies[0].centre + Vec3{0, 1, 0};
    CHECK_FALSE(adapter->arenaBlocksSegment(centre - Vec3{4, 0, 0}, centre + Vec3{4, 0, 0}, .1f));
}

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
