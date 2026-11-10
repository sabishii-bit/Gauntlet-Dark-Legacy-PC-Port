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
#include "game/combat/MissileTarget.h"
#include "game/combat/StrikeHit.h"
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

TEST_CASE("Dreamcast Chimera compatibility cover uses native art and three independent box bodies",
          "[legends][dreamcast-scenery][dreamcast-arena-cover][dreamcast-chimera-cover][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levela5";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    const auto records = formats::LegendsDreamcastScene::parse(bytes);
    // This is an intentional compatibility repair: the DC level omitted the
    // three GC/N64 cover placements, despite retaining their matching model.
    CHECK(std::ranges::none_of(
        records.itemInfos, [](const auto& info) { return info.type == 10 && info.subtype == 41; }));
    auto art = loadLegendsDreamcastArchive(directory.parent_path().parent_path() / "items/levela5");
    REQUIRE(art);
    const auto model = art->models.find("SAFEROCK1L1");
    REQUIRE(model);
    const auto& mesh = art->models.mesh(*model);
    usize nativeVertices = 0;
    for (const auto& part : mesh.parts) {
        nativeVertices += part.indices.size();
    }
    REQUIRE(nativeVertices == 30);
    const auto objectBytes = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(bytes, objectBytes);
    map.textures =
        formats::decodeLegendsDreamcastTextures(objectBytes, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(adapter);
    const auto root = test::assetOrSkip("PLAYERS").parent_path();
    LegendsLevel level{device, map, std::move(adapter), directory, root};
    constexpr std::array<Vec3, 3> kPositions{Vec3{-30.125f, .25f, -16.5f},
                                             Vec3{29.4609375f, .25f, -16.5f}, Vec3{0, 0, -6.5f}};
    const auto targets = level.adapter().arenaDamageTargets();
    const auto bodies = level.adapter().arenaObstacles();
    const auto anchors = level.arenaAnchors();
    REQUIRE(targets.size() == 3);
    REQUIRE(bodies.size() == 3);
    REQUIRE(anchors.size() == 3);
    REQUIRE(level.targets().size() == 3);
    REQUIRE(level.arenaMissileStops().size() == 3);
    for (usize index = 0; index < kPositions.size(); ++index) {
        CAPTURE(index);
        const auto& body = bodies[index];
        const auto& target = targets[index];
        CHECK(target.id == kImportedArenaBase + static_cast<s32>(index));
        CHECK(target.base == kPositions[index]);
        CHECK(target.radius == 6.4f);
        CHECK(target.height == 5);
        REQUIRE(target.surface.size() == 12);
        CHECK(body.centre == kPositions[index]);
        CHECK(body.cylinderRadius == 0);
        CHECK(body.halfAcross == 4.5f);
        CHECK(body.halfAlong == 4.5f);
        CHECK(body.height == 5);
        REQUIRE(body.enemyItem);
        CHECK_FALSE(body.enemyItem->cylindrical);
        const Vec3 centre = kPositions[index] + Vec3{0, 2, 0};
        CHECK(body.pushOut(centre, .5f) != centre);
        CHECK(body.blocksSegment(centre + Vec3{4.4f, 0, 8}, centre + Vec3{4.4f, 0, -8}, .01f));
        CHECK_FALSE(body.blocksSegment(centre + Vec3{6, 0, 8}, centre + Vec3{6, 0, -8}, .01f));
        CHECK_FALSE(body.blocksSegment(centre + Vec3{0, 4, 8}, centre + Vec3{0, 4, -8}, .01f));
        const MissileTarget missile{target.id, target.base, target.radius, target.height,
                                    target.surface};
        StrikeHit shot;
        shot.from = centre + Vec3{0, 0, 8};
        shot.centre = centre;
        shot.radius = .1f;
        shot.damage = 40;
        shot.swept = true;
        CHECK(missile.reachedBy(shot));
        CHECK_FALSE(missile.touches(centre + Vec3{6, 0, 0}, .1f));
        CHECK(level.arenaMissileStops()[index].rockHealth == 100);
        CHECK(level.arenaMissileStops()[index].rockArmor == 10);
        CHECK(placedMeshVertices(device, level.adapter(), mesh, anchors[index]) == nativeVertices);
    }
    const auto allocations = device.texturesCreated;
    CHECK(level.strike(kImportedArenaBase, 10, 0, 0));
    CHECK(level.arenaMissileStops()[0].rockHealth == 99);
    CHECK(level.strike(kImportedArenaBase, 49.75f, 0, 0));
    CHECK(level.arenaMissileStops()[0].rockHealth == 60);
    CHECK(level.strike(kImportedArenaBase, 1000, 0, 0));
    CHECK_FALSE(level.strike(kImportedArenaBase, 1, 0, 0));
    CHECK(level.targets().size() == 2);
    CHECK(level.arenaMissileStops().size() == 2);
    CHECK(level.arenaMissileStops()[0].rock == kImportedArenaBase + 1);
    CHECK_FALSE(level.arenaTargets()[0].active);
    CHECK(placedMeshVertices(device, level.adapter(), mesh, anchors[0]) == 0);
    CHECK(placedMeshVertices(device, level.adapter(), mesh, anchors[1]) == nativeVertices);
    CHECK(device.texturesCreated == allocations);
    auto freshMap = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    const auto reloaded =
        LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, freshMap);
    REQUIRE(reloaded);
    REQUIRE(reloaded->arenaMissileStops().size() == 3);
    CHECK(reloaded->arenaMissileStops()[0].rockHealth == 100);
    CHECK(reloaded->arenaAnchors() == anchors);
    CHECK(placedMeshVertices(device, *reloaded, mesh, anchors[0]) == nativeVertices);
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
