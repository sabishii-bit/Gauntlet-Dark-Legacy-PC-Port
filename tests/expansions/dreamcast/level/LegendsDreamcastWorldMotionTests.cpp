#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/TreePose.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/level/LegendsDreamcastWorldMotion.h"
#include "expansions/dreamcast/scenes/LegendsInitialPose.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsPreview.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/level/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void write16(std::array<u8, 12>& bytes, usize at, u16 value) {
    bytes[at] = static_cast<u8>(value);
    bytes[at + 1] = static_cast<u8>(value >> 8);
}

TEST_CASE("Dreamcast scripted lightning flashes briefly without changing native depth flags",
          "[legends][dreamcast-motion][dreamcast-lightning][assets]") {
    const auto levels =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_regular_file(levels / "levela5/worlds.rom"))
        SKIP("Optional Dreamcast extraction is not installed");
    for (const auto level : {"levela5", "levelf2"}) {
        CAPTURE(level);
        const auto directory = levels / level;
        const auto bytes = readFile(directory / "worlds.rom");
        const auto records = formats::LegendsDreamcastScene::parse(bytes);
        const auto tracks = formats::LegendsSceneMotion::parseDreamcast(bytes);
        const auto authored =
            formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
        auto initial = authored;
        applyDreamcastInitialPose(initial, tracks);
        LegendsDreamcastWorldMotion motion{authored, initial, tracks, records, 1};
        LegendsDreamcastWorldMotion repeat{authored, initial, tracks, records, 1};
        std::vector<usize> lightning;
        for (usize i = 0; i < authored.objects.size(); ++i) {
            const auto& object = authored.objects[i];
            if (object.name.starts_with("A5XPXLNC_LNING") ||
                object.name.starts_with("F2XPNC_LNING")) {
                lightning.push_back(i);
                REQUIRE_FALSE(object.mesh.parts.empty());
                CHECK((object.nativeDrawFlags & object_flags::kNoDepthTest) == 0);
                CHECK((object.nativeDrawFlags & object_flags::kNoDepthWrite) != 0);
                CHECK((object.nativeDrawFlags & object_flags::kAdditive) != 0);
                CHECK(motion.opacity(i) == 0);
                CHECK(std::ranges::none_of(tracks.worldAnimations, [&](const auto& track) {
                    return track.objectIndex == static_cast<s32>(i);
                }));
            }
        }
        REQUIRE(lightning.size() == 4);
        std::array<int, 4> first{}, duration{}, flashes{};
        std::array<bool, 4> previous{};
        for (int tick = 1; tick <= 1800; ++tick) {
            motion.advance(1.f / 60);
            repeat.advance(1.f / 60);
            for (usize i = 0; i < lightning.size(); ++i) {
                const bool visible = motion.opacity(lightning[i]) == 1;
                CHECK(motion.opacity(lightning[i]) == repeat.opacity(lightning[i]));
                CHECK(motion.opacity(lightning[i], .25f) == motion.opacity(lightning[i]));
                if (visible) {
                    if (!previous[i]) {
                        ++flashes[i];
                        if (first[i] == 0)
                            first[i] = tick;
                    }
                    ++duration[i];
                } else if (previous[i]) {
                    // .2 seconds at the native fixed 60Hz clock; permit one
                    // boundary tick for the float scheduler's accumulation.
                    CHECK(duration[i] >= 11);
                    CHECK(duration[i] <= 13);
                    duration[i] = 0;
                }
                previous[i] = visible;
            }
        }
        for (usize i = 0; i < lightning.size(); ++i) {
            CHECK(first[i] >= 300);
            CHECK(first[i] <= 900);
            CHECK(flashes[i] >= 2);
            const auto placement = motion.transform(lightning[i]);
            for (usize column = 0; column < 4; ++column)
                CHECK(glm::distance(placement[column], Mat4{1}[column]) < .0001f);
        }
    }
}

TEST_CASE("Dreamcast ambient parents use the nearest animated node's current origin",
          "[legends][dreamcast-motion][dreamcast-ambient]") {
    formats::LegendsMap authored;
    authored.objects.resize(4);
    authored.objects[0].transform = glm::translate(Mat4{1}, Vec3{2, 0, 0});
    authored.objects[1].transform = Mat4{1};
    authored.objects[2].transform = glm::translate(Mat4{1}, Vec3{8, 0, 0});
    authored.objects[3].transform = Mat4{1};
    formats::LegendsSceneMotion native;
    native.objects.resize(4);
    native.objects[0].position = {2, 0, 0};
    native.objects[1].position = {-2, 0, 0};
    native.objects[2].position = {8, 0, 0};
    native.parents = {-1, 0, -1, -1};
    formats::WorldAnimationRecord key;
    key.objectIndex = 2;
    key.frameCount = 31;
    key.track.flags = 0x10;
    key.track.frames = {0, 30};
    key.track.values = {0, 0};
    native.worldAnimations.push_back(key); // the farther candidate is visited first
    key.objectIndex = 0;
    key.track.values = {0, 6};
    native.worldAnimations.push_back(key);
    auto initial = authored;
    applyDreamcastInitialPose(initial, native);
    LegendsDreamcastWorldMotion motion{authored, initial, native, {}, 1};

    // A child carried by a keyed parent is moving, but it is not itself an
    // animated node. Neither that child nor the static object at the sound
    // origin may win the native nearest-animation search.
    REQUIRE(motion.moves(1));
    const auto near = motion.ambientParent({0, 0, 0});
    REQUIRE(near);
    CHECK(near->first == 0);
    CHECK(near->second == Vec3{2, 0, 0});
    CHECK_FALSE(motion.ambientParent({-9, 0, 0}));   // outside the native ten-unit radius
    CHECK_FALSE(motion.ambientParent({0, 0, 0}, 2)); // strict distance comparison
    CHECK_FALSE(motion.ambientParent({0, 0, 0}, 0));
    CHECK_FALSE(motion.ambientParent({0, 0, 0}, -1));
    CHECK_FALSE(motion.ambientParent({0, 0, 0}, std::numeric_limits<f32>::infinity()));
    CHECK_FALSE(motion.ambientParent({0, 0, 0}, std::numeric_limits<f32>::quiet_NaN()));

    motion.capturePresentation();
    motion.advance(.5f);
    const auto moved = motion.ambientParent({3, 0, 0});
    REQUIRE(moved);
    CHECK(moved->first == 0);
    CHECK(glm::distance(moved->second, Vec3{5, 0, 0}) < .0001f);
    const Vec3 followed{motion.transform(near->first) * Vec4{near->second, 1}};
    CHECK(glm::distance(followed, moved->second) < .0001f);
    const auto far = motion.ambientParent({8, 0, 0});
    REQUIRE(far);
    CHECK(far->first == 2);
    CHECK(far->second == Vec3{8, 0, 0});
}

TEST_CASE("Dreamcast trigger controls keyed child collision and continuous rider transforms",
          "[legends][dreamcast-motion]") {
    formats::LegendsMap authored;
    authored.objects.resize(2);
    authored.objects[0].transform = glm::translate(Mat4{1}, Vec3{10, 0, 0});
    authored.objects[1].transform = authored.objects[0].transform;
    authored.collision.push_back({.vertices = {Vec3{8, 0, -2}, Vec3{12, 0, -2}, Vec3{10, 0, 2}},
                                  .normal = {0, 1, 0},
                                  .owner = 1,
                                  .floor = true});
    formats::LegendsSceneMotion native;
    native.objects.resize(2);
    native.objects[0].position = {10, 0, 0};
    native.objects[0].flags = 0x1000;
    native.objects[1].flags = 0x01001004;
    native.parents = {-1, 0};
    formats::WorldAnimationRecord key;
    key.objectIndex = 0;
    key.frameCount = 31;
    key.track.flags = 0x20;
    key.track.frames = {0, 30};
    key.track.values = {0, 10};
    native.worldAnimations.push_back(key);
    formats::LegendsDreamcastScene scene;
    scene.itemInfos.push_back(
        {.type = 5, .subtype = 24, .radius = 1, .height = 3, .name = "BRIDGEPAD"});
    formats::ItemInstanceRecord switchItem;
    switchItem.info = 0;
    switchItem.position = {10, 0, 0};
    write16(switchItem.params, 2, 2);
    scene.itemInstances.push_back(switchItem);
    auto initial = authored;
    applyDreamcastInitialPose(initial, native);
    LegendsDreamcastWorldMotion motion{authored, initial, native, scene, 1};
    motion.bindSounds([](s32 slot) -> std::optional<std::array<u32, 2>> {
        REQUIRE(slot == 0);
        return std::array<u32, 2>{0x210033, 0x210034};
    });
    motion.advance(.5f);
    CHECK(motion.soundLoops().empty());
    CHECK(motion.offset(1).y == 0);
    const std::array visitors{LegendsPortalVisitor{.position = {10, 0, 0}}};
    motion.touch(visitors);
    REQUIRE(motion.switches()[0].active);
    REQUIRE(motion.soundLoops().size() == 1);
    CHECK(motion.soundLoops()[0].token == 0x210033);
    WorldCollision collision;
    collision.build({{{0, 1, 0}, authored.collision[0].vertices, 1, 4}});
    motion.bindCollision(collision);
    motion.capturePresentation();
    motion.advance(.5f);
    motion.syncCollision(collision);
    CHECK(motion.offset(1).y > 4);
    CHECK(motion.offset(1).y < 6);
    const auto floor = collision.floorAt({10, 5, 0}, 2, 2);
    REQUIRE(floor);
    CHECK(floor->object == 1);
    const auto rider = Vec3{motion.riderTransform(1) * Vec4{10, 1, 0, 1}};
    CHECK(glm::length(rider - Vec3{10, floor->y + 1, 0}) < .001f);
    CHECK(motion.offset(1, .5f).y < motion.offset(1).y);
    motion.advance(1);
    const auto endpoint = motion.riderTransform(1);
    CHECK(motion.soundLoops().empty());
    const auto stopped = motion.takeSounds();
    REQUIRE(stopped.size() == 1);
    CHECK(stopped[0].token == 0x210034);
    motion.advance(1);
    CHECK(motion.takeSounds().empty());
    CHECK(motion.riderTransform(1) == endpoint);
}

TEST_CASE("Dreamcast chained switches preserve whole-party requirements and invisible contacts",
          "[legends][dreamcast-motion]") {
    formats::LegendsMap map;
    map.objects.resize(2);
    formats::LegendsSceneMotion native;
    native.objects.resize(2);
    native.parents = {-1, -1};
    formats::LegendsDreamcastScene scene;
    scene.itemInfos.push_back(
        {.type = 5, .subtype = 24, .radius = 1, .height = 3, .name = "BRIDGEPAD"});
    formats::ItemInstanceRecord first;
    first.info = 0;
    first.params[7] = 1;
    write16(first.params, 2, 2);
    scene.itemInstances.push_back(first);
    auto second = first;
    second.flags = 2;
    second.position = {100, 0, 0};
    second.params[6] = 1;
    second.params[7] = 0;
    write16(second.params, 0, 1);
    write16(second.params, 2, 0x402);
    scene.itemInstances.push_back(second);
    LegendsDreamcastWorldMotion motion{map, map, native, scene, 2};
    std::array visitors{LegendsPortalVisitor{},
                        LegendsPortalVisitor{.player = 1, .position = {20, 0, 0}}};
    motion.touch(visitors);
    CHECK(motion.switches()[0].active);
    CHECK(motion.switches()[1].hidden);
    CHECK_FALSE(motion.switches()[1].active);
    visitors[1].position = {};
    motion.touch(visitors);
    CHECK(motion.switches()[1].active);
}

TEST_CASE("Dreamcast registered height targets honor native speed and closed initial offsets",
          "[legends][dreamcast-motion]") {
    formats::LegendsMap map;
    map.objects.resize(1);
    map.objects[0].transform = glm::translate(Mat4{1}, Vec3{0, 10, 0});
    formats::LegendsSceneMotion native;
    native.objects.resize(1);
    native.objects[0].position = {0, 10, 0};
    native.parents = {-1};
    formats::LegendsDreamcastScene scene;
    scene.itemInfos.push_back(
        {.type = 5, .subtype = 24, .radius = 1, .height = 3, .name = "BRIDGEPAD"});
    formats::ItemInstanceRecord pad;
    pad.info = 0;
    write16(pad.params, 2, 2);
    write16(pad.params, 8, 20);
    write16(pad.params, 10, 100);
    scene.itemInstances.push_back(pad);
    LegendsDreamcastWorldMotion motion{map, map, native, scene, 1};
    CHECK(motion.offset(0).y == 2);
    motion.touch(std::array{LegendsPortalVisitor{}});
    motion.advance(.25f);
    CHECK(motion.offset(0).y == 3);
    motion.advance(5);
    CHECK(motion.offset(0).y == 10);
    motion.advance(.25f);
    CHECK(motion.offset(0).y == 10);
}

TEST_CASE("Dreamcast keyed riders recover from an exactly collapsed intermediate scale",
          "[legends][dreamcast-motion]") {
    formats::LegendsMap map;
    map.objects.resize(1);
    formats::LegendsSceneMotion native;
    native.objects.resize(1);
    native.parents = {-1};
    formats::WorldAnimationRecord key;
    key.objectIndex = 0;
    key.frameCount = 61;
    key.track.flags = 0x700;
    key.track.frames = {0, 30, 60};
    key.track.values = {1, 1, 1, 0, 0, 0, 1, 1, 1};
    native.worldAnimations.push_back(key);
    formats::LegendsDreamcastScene scene;
    scene.itemInfos.push_back(
        {.type = 5, .subtype = 24, .radius = 1, .height = 3, .name = "BRIDGEPAD"});
    formats::ItemInstanceRecord pad;
    pad.info = 0;
    write16(pad.params, 2, 2);
    scene.itemInstances.push_back(pad);
    LegendsDreamcastWorldMotion motion{map, map, native, scene, 1};
    motion.touch(std::array{LegendsPortalVisitor{}});
    motion.advance(1);
    CHECK(glm::determinant(motion.riderTransform(0)) == 0);
    motion.advance(1);
    const auto restored = motion.riderTransform(0);
    for (Mat4::length_type column = 0; column < 4; ++column)
        for (Mat4::length_type row = 0; row < 4; ++row)
            CHECK(std::isfinite(restored[column][row]));
    CHECK(restored == Mat4{1});
}

TEST_CASE("Dreamcast item grounding uses initialized target floors and preserves its binding basis",
          "[legends][dreamcast-motion][dreamcast-placement]") {
    formats::LegendsMap map;
    map.objects.resize(1);
    map.collision.push_back({.vertices = {Vec3{-3, 0, -3}, Vec3{3, 0, -3}, Vec3{0, 0, 3}},
                             .normal = {0, 1, 0},
                             .owner = 0,
                             .floor = true});
    formats::LegendsSceneMotion native;
    native.objects.resize(1);
    native.objects[0].flags = 0x1004;
    native.parents = {-1};
    formats::LegendsDreamcastScene scene;
    scene.itemInfos.push_back({.type = 5, .subtype = 24, .radius = 1, .height = 3});
    formats::ItemInstanceRecord pad;
    pad.info = 0;
    pad.position = {0, 2, 0};
    write16(pad.params, 2, 2);
    write16(pad.params, 8, 20); // ProcessItemWobjs places the initial floor at Y2.
    write16(pad.params, 10, 100);
    scene.itemInstances.push_back(pad);
    LegendsDreamcastWorldMotion motion{map, map, native, scene, 1};
    const formats::ItemInfoRecord grounded{.type = 2};
    const formats::ItemInfoRecord airborne{.type = 2, .collisionFlags = 1};
    CHECK(motion.itemPosition(grounded, {0, 1, 0}).y == 2.1f);
    CHECK(motion.itemPosition(grounded, {0, 3, 0}).y == 2.1f);
    CHECK(motion.itemPosition(airborne, {0, 1, 0}).y == 1);
    CHECK(motion.itemPosition(grounded, {20, 5, 20}).y == 5.1f);
    CHECK(motion.riderTransform(0) == Mat4{1});
    CHECK(motion.switchPosition(0).y == 2.1f);
    motion.touch(std::array{LegendsPortalVisitor{.position = motion.switchPosition(0)}});
    motion.advance(.5f);
    const Vec3 carried = Vec3{motion.riderTransform(0) * Vec4{0, 2.1f, 0, 1}};
    CHECK(std::abs(carried.y - 4.1f) < .0001f);
    CHECK(std::abs(motion.switchPosition(0).y - carried.y) < .0001f);
}

TEST_CASE("Shipped Dreamcast world tracks have invertible initial animation bases",
          "[legends][dreamcast-motion][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_directory(root / "levels"))
        SKIP("Optional Dreamcast extraction is not installed");
    usize levels = 0, tracks = 0;
    for (const auto& directory : std::filesystem::directory_iterator(root / "levels")) {
        const auto level = directory.path().filename().string();
        const auto metadata = formats::dreamcastLegendsWorldDataFile(level);
        if (metadata.empty() || !std::filesystem::is_regular_file(root / metadata) ||
            !formats::dreamcastLegendsFixtureScope(readFile(root / metadata), level))
            continue;
        const auto path = directory.path() / "worlds.rom";
        if (!std::filesystem::is_regular_file(path))
            continue;
        const auto motion = formats::LegendsSceneMotion::parseDreamcast(readFile(path));
        ++levels;
        for (const auto& animation : motion.worldAnimations) {
            if ((motion.objects[static_cast<usize>(animation.objectIndex)].flags & 0x300000U) ==
                0x300000U)
                continue;
            ++tracks;
            const TrackInfo keys{animation.track.node, animation.track.flags,
                                 animation.track.frames, animation.track.values};
            const auto pose = TreePose::sample(keys, animation.startFrame);
            INFO(level << " world object " << animation.objectIndex);
            CHECK(pose.scale.x != 0);
            CHECK(pose.scale.y != 0);
            CHECK(pose.scale.z != 0);
        }
    }
    CHECK(levels > 20);
    CHECK(tracks > 50);
}

TEST_CASE("Dreamcast B1 displays native pads and carries RUNEB1 throughout its pedestal reveal",
          "[legends][dreamcast-motion][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::exists(directory / "worlds.rom"))
        SKIP("Optional Dreamcast extraction is not installed");
    const auto bytes = readFile(directory / "worlds.rom");
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(scene);
    const auto* motion = scene->dreamcastWorldMotion();
    REQUIRE(motion);
    REQUIRE(motion->switches().size() == 6);
    usize plates = 0;
    for (usize i = 0; i < scene->fixtureCount(); ++i) {
        const auto figure =
            scene->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(i));
        if (figure && figure->name == "BRIDGEPAD" && figure->visible)
            ++plates;
    }
    CHECK(plates == 2); // four authored flag2 volumes remain invisible, but active
    const auto rune = std::ranges::find(scene->pickups(), 144U, &LegendsPickup::nativeIndex);
    REQUIRE(rune != scene->pickups().end());
    const auto native = formats::LegendsDreamcastScene::parse(bytes);
    const Vec3 authoredRune = native.itemInstances[144].position;
    const auto support = motion->supportAt(authoredRune);
    REQUIRE(support);
    REQUIRE(support->object == 231);
    REQUIRE(motion->moves(support->object));
    const Vec3 before = rune->position;
    std::vector<LegendsPortalVisitor> visitors;
    // Visible pad138 targets the actual supporting lift230/FLOOR#27(231).
    // Hidden pads141..143 separately animate the nested decorative pedestal.
    visitors.push_back({.position = motion->switchPosition(0)});
    for (usize i = 3; i < 6; ++i)
        visitors.push_back({.position = motion->switchPosition(i)});
    scene->touchSwitches(visitors);
    REQUIRE(motion->switches()[0].active);
    for (usize i = 3; i < 6; ++i)
        REQUIRE(motion->switches()[i].active);
    scene->capturePresentation();
    scene->update(.5f);
    const Vec3 middle = rune->position;
    const Vec3 expected = Vec3{motion->riderTransform(support->object) * Vec4{before, 1}};
    CHECK(glm::length(middle - expected) < .002f);
    CHECK(glm::length(middle - before) > .1f);
    CHECK(glm::length(middle - before) < 30.f);
    CHECK(scene->worldObjectTransform(245) != Mat4{1});
    scene->update(2);
    const Vec3 after = rune->position;
    CHECK(glm::length(after - middle) > .1f);
    for (s32 tick = 0; tick < 60; ++tick)
        scene->update(1.f / 60);
    CHECK(glm::length(rune->position - after) < .002f);
}

TEST_CASE("Dreamcast B1 grounded containers and RUNEB1 stay on floors through LevelWorld updates",
          "[legends][dreamcast-motion][dreamcast-placement][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::exists(directory / "worlds.rom"))
        SKIP("Optional Dreamcast extraction is not installed");
    const auto common = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto bytes = readFile(directory / "worlds.rom");
    const auto records = formats::LegendsDreamcastScene::parse(bytes);
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(adapter);
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    auto native =
        std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, common);
    LevelWorld world;
    LevelRef reference;
    reference.realmId = -1;
    reference.name = "dreamcast:levelb1";
    REQUIRE(world.loadImported(device, common, reference,
                               WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                               traversal.collision(), native));
    const auto& scene = native->adapter();
    const auto* motion = scene.dreamcastWorldMotion();
    REQUIRE(motion);
    // These are the exact selected object identities in the player's report,
    // not guessed native placement indices (hidden chest variants add figures).
    for (const s32 id : {16777217, 16777235, 16777247}) {
        const auto object = native->object(id);
        REQUIRE(object);
        INFO(id << ' ' << object->name << " at " << object->position.x << ',' << object->position.y
                << ',' << object->position.z);
        const auto placed = std::ranges::find_if(records.itemInstances, [&](const auto& item) {
            return glm::length(Vec2{item.position.x - object->position.x,
                                    item.position.z - object->position.z}) < .001f;
        });
        REQUIRE(placed != records.itemInstances.end());
        const auto floor = world.collision().restingFloorAt(placed->position, 4, 10, 1);
        REQUIRE(floor);
        INFO("Authored Y " << placed->position.y << " floor Y " << floor->y);
        CHECK(std::abs(object->position.y - (floor->y + .1f)) < .002f);
    }
    const auto rune = std::ranges::find(scene.pickups(), 144U, &LegendsPickup::nativeIndex);
    REQUIRE(rune != scene.pickups().end());
    const auto floor =
        world.collision().restingFloorAt(records.itemInstances[144].position, 4, 10, 1);
    REQUIRE(floor);
    REQUIRE(floor->object == 231);
    const Vec3 before = rune->position;
    CHECK(std::abs(before.y - (floor->y + .1f)) < .002f);
    std::optional<s32> runeFigure;
    for (usize index = 0; index < scene.fixtureCount(); ++index) {
        const auto id = LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(index);
        const auto figure = native->object(id);
        if (figure && figure->name == "RUNEB1")
            runeFigure = id;
    }
    REQUIRE(runeFigure);
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, motion->switchPosition(0), 0);
    std::array<PlayerActor*, 1> players{&actor};
    native->updateParty(players, 1.f / 60, false);
    REQUIRE(motion->switches()[0].active);
    for (s32 tick = 1; tick <= 120; ++tick) {
        world.capturePresentation();
        world.update(1.f / 60);
        native->updateParty(players, 1.f / 60, false);
        const auto contact = world.collision().restingFloorAt(rune->position, 1, 1, 1);
        REQUIRE(contact);
        REQUIRE(contact->object == 231);
        CAPTURE(tick, rune->position.y, contact->y);
        CHECK(std::abs(rune->position.y - (contact->y + .1f)) < .002f);
        const auto figure = native->object(*runeFigure);
        REQUIRE(figure);
        CHECK(glm::distance(figure->position, rune->position) < .002f);
        if (tick == 30)
            CHECK(rune->position.y > before.y + .1f);
    }
    CHECK(rune->position.y > before.y + 1.f);
}

TEST_CASE("Dreamcast F1 bridge render and collision start at the first key and rise together",
          "[legends][dreamcast-motion][dreamcast-f1][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelf1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom"))
        SKIP("Optional Dreamcast extraction is not installed");
    const auto bytes = readFile(directory / "worlds.rom");
    const auto records = formats::LegendsDreamcastScene::parse(bytes);
    const auto tracks = formats::LegendsSceneMotion::parseDreamcast(bytes);
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    REQUIRE(map.objects.at(425).name == "F1ELEV010");
    REQUIRE(map.objects.at(426).name == "F1FLOOR#23");
    const auto key =
        std::ranges::find(tracks.worldAnimations, 425, &formats::WorldAnimationRecord::objectIndex);
    REQUIRE(key != tracks.worldAnimations.end());
    CHECK(key->startFrame == 0);
    CHECK(key->frameCount == 80);
    // The nearby F1ROCKFALL#06 is a separate native type10/subtype40 item.
    // It must not replace the switch-driven floor's authored first pose.
    REQUIRE(records.itemInstances.at(12).name == "F1ROCKFALL#06");
    const auto& rubble = records.itemInfos.at(records.itemInstances[12].info);
    CHECK(rubble.type == 10);
    CHECK(rubble.subtype == 40);
    CHECK(rubble.collisionFlags == 1);
    CHECK(std::abs(records.itemInstances[12].position.y - 3.97f) < .0001f);

    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(adapter);
    const auto* motion = adapter->dreamcastWorldMotion();
    REQUIRE(motion);
    const auto pad = std::ranges::find(motion->switches(), 399U,
                                       &LegendsDreamcastWorldMotion::Switch::nativeIndex);
    REQUIRE(pad != motion->switches().end());
    REQUIRE(pad->target == 425);
    const usize padIndex = static_cast<usize>(pad - motion->switches().begin());
    // Native DoWorldAnimSub samples the initial key (.08843), not the final
    // key. This is the child's actual world matrix, including its parent.
    CHECK(std::abs(map.objects[426].transform[3].y - (-.21000004f + .08843f)) < .0001f);

    std::vector<CollisionTriangle> floorTriangles;
    for (const auto& source : map.collision) {
        if (source.owner == 426 && source.floor)
            floorTriangles.push_back({source.normal, source.vertices, source.owner, 4});
    }
    REQUIRE(floorTriangles.size() == 5);
    const auto initialTriangles = floorTriangles;
    WorldCollision collision;
    collision.build(std::move(floorTriangles));
    motion->bindCollision(collision);

    // Keep all native identities and the real adapter, but mark just this
    // floor's submitted vertices so fixture draws cannot satisfy the check.
    const Color marker = Color::rgba(1, 2, 3, 255);
    for (usize i = 0; i < map.objects.size(); ++i) {
        if (i == 426) {
            for (auto& vertex : map.objects[i].mesh.vertices)
                vertex.color = marker;
        } else {
            map.objects[i].mesh = {};
            map.objects[i].partLightmaps.clear();
            map.objects[i].partCullBack.clear();
        }
    }
    LegendsPreview preview;
    preview.open(device, map, std::move(adapter));
    const auto rendered = [&] {
        device.draws.clear();
        preview.drawWorld(device, Mat4{1}, WorldCamera{}, TreeModel::Pass::All, -1);
        std::vector<Vec3> vertices;
        for (const auto& draw : device.draws)
            for (const auto& vertex : draw.vertices)
                if (vertex.color == marker)
                    vertices.push_back(Vec3{draw.transform * Vec4{vertex.position, 1}});
        REQUIRE(vertices.size() == 15);
        return vertices;
    };
    const auto verify = [&] {
        const auto vertices = rendered();
        const Mat4 movement = motion->transform(426);
        for (const auto& triangle : initialTriangles) {
            Vec3 centre{0};
            for (const auto& vertex : triangle.vertices) {
                const Vec3 expected{movement * Vec4{vertex, 1}};
                centre += expected / 3.f;
                const auto nearest = std::ranges::min(vertices, {}, [&](const Vec3& drawn) {
                    return glm::distance(expected, drawn);
                });
                // Collision edge coordinates are quantized to 1/64 units;
                // render geometry retains authored floats.
                CHECK(glm::distance(expected, nearest) < .04f);
            }
            const auto floor = collision.floorAt(centre, .01f, .01f);
            REQUIRE(floor);
            CHECK(floor->object == 426);
            CHECK(std::abs(floor->y - centre.y) < .002f);
        }
        return vertices;
    };
    const auto initial = verify();
    preview.adapter()->update(1);
    motion->syncCollision(collision);
    CHECK(rendered() == initial);
    preview.adapter()->touchSwitches(
        std::array{LegendsPortalVisitor{.position = motion->switchPosition(padIndex)}});
    REQUIRE(motion->switches()[padIndex].active);
    preview.adapter()->capturePresentation();
    preview.adapter()->update(.5f);
    motion->syncCollision(collision);
    const auto halfway = verify();
    CHECK(halfway.front().y > initial.front().y);
    preview.adapter()->update(3);
    motion->syncCollision(collision);
    const auto final = verify();
    CHECK(final.front().y > halfway.front().y);
    CHECK(final.front().y > initial.front().y + 1);
    preview.adapter()->update(1);
    CHECK(rendered() == final);
}
} // namespace
