#include <algorithm>
#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/dreamcast/level/LegendsDreamcastScenery.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/combat/DamageTypes.h"
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

TEST_CASE("Dreamcast shoot walls share transformed attack and collision surfaces",
          "[legends][dreamcast-scenery]") {
    const formats::ItemInfoRecord info{
        .type = 10, .subtype = 42, .collisionType = 4, .armor = 1, .hitPoints = 25};
    formats::ItemInstanceRecord item;
    item.position = {10, 5, 20};
    item.rotation.y = kHalfPi;
    item.triangleIndex = 0;
    item.triangleCount = 1;
    const std::array faces{
        formats::WorldCollisionTriangle{{0, 0, 1}, {Vec3{-2, 0, 0}, Vec3{2, 0, 0}, Vec3{0, 4, 0}}}};
    constexpr s32 kObject = 0x01000007;
    LegendsDreamcastScenery scenery;
    scenery.add(info, item, 7, kObject, faces);
    const auto targets = scenery.targets();
    REQUIRE(targets.size() == 1);
    const auto& target = targets[0];
    CHECK(target.valid());
    REQUIRE(target.surface.size() == 1);
    const MissileTarget missile{target.id, target.base, target.radius, target.height,
                                target.surface};
    CHECK(missile.touches(item.position + Vec3{0, 1, 0}, .1f));
    CHECK_FALSE(missile.touches(item.position + Vec3{5, 1, 0}, .1f));
    const Mat4 transform = legendsDreamcastItemPlacement(item.position, item.rotation);
    for (usize i = 0; i < 3; ++i) {
        CHECK(glm::distance(target.surface[0].vertices[i],
                            Vec3{transform * Vec4{faces[0].vertices[i], 1}}) < .00001f);
    }
    CHECK(target.surface[0].object == kObject);
    CHECK(scenery.targetForObject(kObject) == target.id);
    CHECK_FALSE(scenery.targetForObject(7));
    WorldCollision collision;
    scenery.bindCollision(collision);
    CHECK(collision.triangleCount() == 1);
    const Vec3 point = item.position + Vec3{0, 1, 0};
    CHECK(collision.resolveWalls(point, .5f, point.y, point.y + 2) != point);
    CHECK_FALSE(scenery.strike(target.id, std::numeric_limits<f32>::infinity(), 0));
    CHECK_FALSE(scenery.strike(target.id, 100, damage::kMagic));
    CHECK(scenery.strike(target.id, 10.75f, 0));
    CHECK(scenery.health(target.id) == 16);
    CHECK(scenery.whiteBlend(7) == 1);
    CHECK(scenery.whiteBlend(8) == 0);
    scenery.update(1.0f / 30);
    CHECK(scenery.whiteBlend(7) == 0);
    CHECK(scenery.strike(target.id, 100, 0));
    scenery.syncCollision(collision);
    CHECK_FALSE(collision.solid(kObject));
    CHECK(collision.resolveWalls(point, .5f, point.y, point.y + 2) == point);
    CHECK_FALSE(scenery.visible(7));
    CHECK(scenery.visible(8));
    CHECK(scenery.targets().empty());
    CHECK_FALSE(scenery.strike(target.id, 1, 0));

    item.triangleIndex = 1;
    CHECK_THROWS_AS(scenery.add(info, item, 8, kObject + 1, faces), FormatError);
    item.triangleIndex = 0;
    auto badFaces = faces;
    badFaces[0].vertices[0].x = std::numeric_limits<f32>::quiet_NaN();
    CHECK_THROWS_AS(scenery.add(info, item, 8, kObject + 1, badFaces), FormatError);
    CHECK_THROWS_AS(scenery.add(info, item, 8, kObject, faces), FormatError);
}

TEST_CASE("Every extracted Dreamcast shoot wall imports its native triangle body and HP",
          "[legends][dreamcast-scenery][assets]") {
    const auto root =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_directory(root)) {
        SKIP("Dreamcast extraction is not installed");
    }
    usize walls = 0;
    usize markers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto path = entry.path() / "worlds.rom";
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        const auto bytes = readFile(path);
        if (bytes.size() < 96) {
            continue;
        }
        const auto world = formats::WorldFile::parse(bytes);
        LegendsDreamcastScenery scenery;
        for (usize index = 0; index < world.itemInstances.size(); ++index) {
            const auto& item = world.itemInstances[index];
            const auto& info = world.itemInfos.at(static_cast<usize>(item.info));
            if (info.type != 10 || info.subtype != 42) {
                continue;
            }
            CAPTURE(path, index, item.name);
            if (item.triangleCount == 0) {
                // H3's trigger-only placement has neither mesh nor collision;
                // the loader does not turn it into a physical attack target.
                CHECK(item.name == "H3SHOOTWALL_UP");
                ++markers;
                continue;
            }
            const s32 object = 0x01000000 + static_cast<s32>(index);
            scenery.add(info, item, index, object, world.collision);
            const auto target = scenery.targets().back();
            CHECK(target.valid());
            CHECK(target.surface.size() == static_cast<usize>(item.triangleCount));
            CHECK(scenery.health(target.id) == info.hitPoints);
            CHECK(scenery.visible(index));
            ++walls;
        }
    }
    CHECK(walls == 199);
    CHECK(markers == 1);
}

TEST_CASE("Valley of Fire shoot wall breaks through imported projectile world-hit routing",
          "[legends][dreamcast-scenery][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Dreamcast extraction is not installed");
    }
    const auto world = readFile(directory / "worlds.rom");
    const auto objects = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(world, objects);
    map.textures =
        formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    auto targets = scene->sceneryTargets();
    REQUIRE(targets.size() == 1);
    const auto& target = targets[0];
    REQUIRE(target.surface.size() == 2);
    const s32 object = target.surface[0].object;
    const auto figure = scene->fixtureObject(object);
    REQUIRE(figure);
    CHECK(figure->name == "B1SHOOTWALL1");
    CHECK(figure->visible);
    CHECK(glm::distance(target.base, figure->position) < 10);
    WorldCollision collision;
    scene->bindCollision(collision);
    CHECK(collision.solid(object));
    CHECK(scene->strikeWorldObject(object, 10, 0));
    CHECK(scene->fixtureObject(object)->visible);
    CHECK(scene->strikeWorldObject(object, 100, 0));
    scene->syncCollision(collision);
    CHECK_FALSE(scene->fixtureObject(object)->visible);
    CHECK_FALSE(collision.solid(object));
    CHECK(scene->sceneryTargets().empty());
    CHECK_FALSE(scene->strikeWorldObject(object, 100, 0));
    const auto sounds = scene->takeSounds();
    REQUIRE_FALSE(sounds.empty());
    CHECK(sounds.back().token == 0x27);
}
} // namespace
