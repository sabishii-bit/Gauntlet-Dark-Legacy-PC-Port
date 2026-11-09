#include <algorithm>
#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/level/LegendsDreamcastPortals.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "formats/AnimationTree.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::vector<u8> roster(s32 world, std::span<const std::string_view> codes) {
    const usize levels = 64, directory = levels + codes.size() * 144;
    std::vector<u8> bytes(directory + 24);
    const auto word = [&](usize at, u32 value) {
        for (u32 byte = 0; byte < 4; ++byte)
            bytes[at + byte] = static_cast<u8>(value >> (byte * 8));
    };
    word(0, static_cast<u32>(directory));
    word(4, 2);
    word(8, static_cast<u32>(world));
    word(directory, 0x57524c44);
    word(directory + 4, 8);
    word(directory + 8, 1);
    word(directory + 12, 0x4c45564c);
    word(directory + 16, static_cast<u32>(levels));
    word(directory + 20, static_cast<u32>(codes.size()));
    for (usize i = 0; i < codes.size(); ++i) {
        REQUIRE(codes[i].size() == 2);
        bytes[levels + i * 144 + 8] = static_cast<u8>(codes[i][0]);
        bytes[levels + i * 144 + 9] = static_cast<u8>(codes[i][1]);
    }
    return bytes;
}

TEST_CASE("Dreamcast linked stages defer their tally and shop until run completion",
          "[legends][dreamcast-portals][legends-completion]") {
    CHECK(dreamcastContinuesRun("levele1", "levele2"));
    CHECK(dreamcastContinuesRun("LEVELF1", "LevelF2"));
    CHECK(dreamcastContinuesRun("levelh1", "levelh4"));
    CHECK(dreamcastContinuesRun("LEVELH2", "LevelH5"));
    CHECK(dreamcastContinuesRun("levelh3", "levelh6"));
    for (const auto source : {"levele1", "levelf1", "levele2", "levelf2", "levelb1", "levelt1"})
        CHECK_FALSE(dreamcastContinuesRun(source, "levelt1"));
    CHECK_FALSE(dreamcastContinuesRun("levele1", "levelf2"));
    CHECK_FALSE(dreamcastContinuesRun("levelf1", "levele2"));
    CHECK_FALSE(dreamcastContinuesRun("levelt1", "levele2"));
    CHECK_FALSE(dreamcastContinuesRun("levelb5", "levelb6"));
    CHECK_FALSE(dreamcastContinuesRun("../levele1", "levele2"));
    CHECK_FALSE(dreamcastContinuesRun("levele1", ""));
    CHECK_FALSE(dreamcastContinuesRun("levelh1", "levelh5"));
    CHECK_FALSE(dreamcastContinuesRun("levelh2", "levelh6"));
    CHECK_FALSE(dreamcastContinuesRun("levelh3", "levelh4"));
    CHECK_FALSE(dreamcastContinuesRun("levelh4", "levelt1"));
    CHECK_FALSE(dreamcastContinuesRun("levelh5", "levelt1"));
    CHECK_FALSE(dreamcastContinuesRun("levelh6", "levelt1"));
}

TEST_CASE("Dreamcast portal policy follows its own realm gates and stage ordering",
          "[legends][dreamcast-portals]") {
    LegendsDreamcastPortalPolicy policy;
    policy.addWorld(roster(2, std::array<std::string_view, 3>{"B1", "B2", "B3"}));
    policy.addWorld(roster(7, std::array<std::string_view, 2>{"G1", "G2"}));
    policy.addWorld(roster(1, std::array<std::string_view, 6>{"A1", "A1", "A3", "A4", "A5", "A6"}));
    policy.addWorld(roster(5, std::array<std::string_view, 1>{"E1"}));
    policy.addWorld(roster(6, std::array<std::string_view, 1>{"F1"}));
    LegendsProgress progress;
    CHECK(policy.allows("levelb1", progress));
    CHECK_FALSE(policy.allows("levelb2", progress));
    CHECK_FALSE(policy.allows("levelg1", progress));
    CHECK_FALSE(policy.allows("levele1", progress));
    CHECK_FALSE(policy.allows("levelf1", progress));
    REQUIRE(progress.recordCompleted("levelb1"));
    CHECK(policy.allows("levelb2", progress));
    CHECK_FALSE(policy.allows("levelb3", progress));
    progress.obelisks = 0x38;
    CHECK(policy.allows("levelg1", progress));
    CHECK_FALSE(policy.allows("levelg2", progress));
    CHECK_FALSE(policy.allows("levela1", progress));
    REQUIRE(progress.recordCompleted("levelg1"));
    CHECK(policy.allows("levela1", progress));
    CHECK(policy.find("levela1")->stage == 0);
    CHECK(policy.find("levela6")->order < policy.find("levela5")->order);
    progress.shards = 0x1e;
    CHECK(policy.allows("levele1", progress));
    progress.runestones = 0x91fff;
    CHECK(policy.allows("levelf1", progress));
    CHECK_FALSE(policy.allows("../levelb1", progress));
    CHECK_FALSE(policy.allows("mount1", progress));
}

TEST_CASE("Dreamcast malformed portal metadata is rejected atomically",
          "[legends][dreamcast-portals]") {
    LegendsDreamcastPortalPolicy policy;
    auto bytes = roster(2, std::array<std::string_view, 2>{"B1", "B2"});
    bytes[64 + 144 + 8] = '/';
    CHECK_THROWS_AS(policy.addWorld(bytes), FormatError);
    CHECK(policy.stages().empty());
    bytes.resize(8);
    CHECK_THROWS_AS(policy.addWorld(bytes), FormatError);
}

TEST_CASE("Dreamcast exit parameters resolve authored codes not arbitrary paths",
          "[legends][dreamcast-portals]") {
    LegendsDreamcastPortalPolicy policy;
    policy.addWorld(roster(2, std::array<std::string_view, 1>{"B1"}));
    formats::ItemInfoRecord info;
    info.type = 9;
    info.radius = 3;
    formats::ItemInstanceRecord instance;
    instance.params[4] = 'B';
    instance.params[5] = '1';
    const auto portal = dreamcastLegendsPortal(info, instance, 42, policy);
    REQUIRE(portal);
    CHECK(portal->map == "levelb1");
    CHECK(portal->nativeIndex == 42);
    CHECK(touchesDreamcastPortal(*portal, Vec3{3, 0, 0}, .5f, 1));
    CHECK_FALSE(touchesDreamcastPortal(*portal, Vec3{0, 4, 0}, .5f, 1));
    CHECK_FALSE(touchesDreamcastPortal(*portal, Vec3{0}, -1, 1));
    CHECK_FALSE(touchesDreamcastPortal(*portal, Vec3{0}, .5f, 0));
    instance.params[0] = 1;
    CHECK_FALSE(dreamcastLegendsPortal(info, instance, 42, policy));
    instance.params[0] = 0;
    instance.params[4] = 'Z';
    CHECK_FALSE(dreamcastLegendsPortal(info, instance, 42, policy));
}

TEST_CASE("Dreamcast portal animation waits for the whole party and never repeats departure",
          "[legends][dreamcast-portals]") {
    LegendsDreamcastPortalMotion motion{{0, 0, 16, 56, 16}};
    for (s32 tick = 0; tick < 180; ++tick) {
        CHECK_FALSE(motion.advance(1.0f / 60, 1, 3, true));
    }
    REQUIRE(motion.phase() == LegendsDreamcastPortalMotion::Phase::Active);
    for (s32 tick = 0; tick < 30; ++tick) {
        CHECK_FALSE(motion.advance(1.0f / 60, 3, 3, true));
    }
    bool departed = false;
    for (s32 tick = 0; tick < 60; ++tick) {
        departed |= motion.advance(1.0f / 60, 3, 3, true);
    }
    CHECK(departed);
    CHECK_FALSE(motion.advance(1, 3, 3, true));
    motion.reset();
    CHECK_FALSE(motion.advance(1, 1, 1, false));
    CHECK(motion.phase() == LegendsDreamcastPortalMotion::Phase::Idle);
    CHECK_FALSE(motion.advance(std::numeric_limits<f32>::quiet_NaN(), 1, 1, true));
    CHECK_FALSE(motion.advance(1, 2, 1, true));
    CHECK_FALSE(motion.advance(.1f, 1, 1, true));
    CHECK_FALSE(motion.advance(1, 0, 1, true));
    CHECK(motion.phase() == LegendsDreamcastPortalMotion::Phase::Idle);
}

TEST_CASE("Dreamcast portal timers round native thirty-Hertz animation duration first",
          "[legends][dreamcast-portals]") {
    CHECK(LegendsDreamcastPortalMotion::sequenceTicks(15, 15) == 16);
    CHECK(LegendsDreamcastPortalMotion::sequenceTicks(28, 30) == 56);
    CHECK(LegendsDreamcastPortalMotion::sequenceTicks(15, 0) == 30);
    CHECK(LegendsDreamcastPortalMotion::sequenceTicks(0, 30) == 0);
    CHECK(LegendsDreamcastPortalMotion::sequenceTicks(-1, 30) == 0);
    CHECK(LegendsDreamcastPortalMotion::sequenceTicks(std::numeric_limits<s32>::max(),
                                                      std::numeric_limits<s32>::max()) == 3600);
}

TEST_CASE("Dreamcast transport completes ACTIVE2 and ACTIVE3 before admitting departure",
          "[legends][dreamcast-portals]") {
    using Phase = LegendsDreamcastPortalMotion::Phase;
    LegendsDreamcastPortalMotion motion{{0, 0, 16, 56, 16}};
    // Two phase-entry updates, then the 16/56/16 native timer sixtieths.
    // The old implementation departed during ACTIVE2, without ACTIVE3.
    for (s32 tick = 0; tick < 89; ++tick) {
        INFO("sixtieth " << tick + 1);
        CHECK_FALSE(motion.advance(1.0f / 60, 1, 1, true));
        if (tick == 72)
            CHECK(motion.phase() == Phase::Active);
        if (tick >= 73)
            CHECK(motion.phase() == Phase::Closing);
    }
    CHECK(motion.advance(1.0f / 60, 1, 1, true));
    CHECK_FALSE(motion.advance(1, 1, 1, true));
}

TEST_CASE("Dreamcast transport timing survives partial parties and render cadence",
          "[legends][dreamcast-portals]") {
    using Phase = LegendsDreamcastPortalMotion::Phase;
    LegendsDreamcastPortalMotion at30{{0, 0, 16, 56, 16}};
    LegendsDreamcastPortalMotion at60{{0, 0, 16, 56, 16}};
    LegendsDreamcastPortalMotion at120{{0, 0, 16, 56, 16}};
    for (s32 tick = 0; tick < 120; ++tick) {
        CHECK_FALSE(at30.advance(1.0f / 30, 1, 3, true));
        for (s32 half = 0; half < 2; ++half)
            CHECK_FALSE(at60.advance(1.0f / 60, 1, 3, true));
        for (s32 quarter = 0; quarter < 4; ++quarter)
            CHECK_FALSE(at120.advance(1.0f / 120, 1, 3, true));
        CHECK(at30.phase() == at60.phase());
        CHECK(at30.phase() == at120.phase());
    }
    REQUIRE(at30.phase() == Phase::Active);
    for (s32 tick = 0; tick < 31; ++tick) {
        const bool departed30 = at30.advance(1.0f / 30, 3, 3, true);
        bool departed60 = false, departed120 = false;
        for (s32 half = 0; half < 2; ++half)
            departed60 |= at60.advance(1.0f / 60, 3, 3, true);
        for (s32 quarter = 0; quarter < 4; ++quarter)
            departed120 |= at120.advance(1.0f / 120, 3, 3, true);
        CHECK(departed30 == departed60);
        CHECK(departed30 == departed120);
        CHECK(departed30 == (tick == 30));
    }

    // A player leaving during the final sequence must not retain departure
    // credit while the remaining player waits or on the next empty update.
    at60.reset();
    for (s32 tick = 0; tick < 120 && at60.phase() != Phase::Closing; ++tick)
        CHECK_FALSE(at60.advance(1.0f / 60, 3, 3, true));
    REQUIRE(at60.phase() == Phase::Closing);
    for (s32 tick = 0; tick < 120; ++tick)
        CHECK_FALSE(at60.advance(1.0f / 60, 1, 3, true));
    CHECK(at60.phase() == Phase::Active);
    for (s32 tick = 0; tick < 120; ++tick)
        CHECK_FALSE(at60.advance(1.0f / 60, 0, 3, true));
    CHECK(at60.phase() == Phase::Idle);
}

TEST_CASE("Native Dreamcast tower portals travel with independent party progress",
          "[legends][dreamcast-portals][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelt1";
    if (!std::filesystem::is_directory(directory))
        SKIP("Dreamcast extraction is not installed");
    const auto animations = formats::AnimationFile::parse(
        readFile(directory.parent_path().parent_path() / "items/levelT/anim.rom"));
    const auto portalTree = animations.find("EXIT_PORTAL");
    REQUIRE(portalTree);
    const auto& sequences = animations.trees[*portalTree].sequences;
    REQUIRE(sequences.size() == 5);
    const std::array<s32, 5> nativeTicks{0, 0, 16, 56, 16};
    for (usize i = 0; i < sequences.size(); ++i) {
        CHECK_FALSE(sequences[i].repeats);
        CHECK(LegendsDreamcastPortalMotion::sequenceTicks(
                  sequences[i].frameCount, sequences[i].frameRate) == nativeTicks[i]);
    }
    const auto objects = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), objects);
    map.textures =
        formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map, 2);
    REQUIRE(scene->dreamcastPortals().size() == 27);
    const auto native = formats::LegendsDreamcastScene::parse(readFile(directory / "worlds.rom"));
    REQUIRE(native.itemInstances.size() > 24);
    CHECK(native.itemInstances[24].params[4] == 'E');
    CHECK(native.itemInstances[24].params[5] == '1');
    REQUIRE((native.itemInstances[24].flags & 2U) != 0);
    CharacterSave first, second;
    first.character = 0;
    second.character = 1;
    const std::array<const CharacterSave*, 2> party{&first, &second};
    scene->synchronizeProgress(party);
    const auto index = [&](std::string_view name) {
        const auto portals = scene->dreamcastPortals();
        const auto at = std::ranges::find(portals, name, &LegendsDreamcastPortal::map);
        REQUIRE(at != portals.end());
        return static_cast<usize>(at - portals.begin());
    };
    const usize mountain = index("levelb1"), next = index("levelb2"), temple = index("levele1");
    CHECK(scene->dreamcastPortalUnlocked(mountain));
    CHECK_FALSE(scene->dreamcastPortalUnlocked(next));
    CHECK(scene->dreamcastPortals()[temple].nativeIndex == 24);
    CHECK_FALSE(scene->dreamcastPortalUnlocked(temple));
    first.progress().legendsProgress(LegendsPlatform::N64).recordCompleted("levelb1");
    scene->synchronizeProgress(party);
    CHECK_FALSE(scene->dreamcastPortalUnlocked(next));
    second.progress().legendsProgress(LegendsPlatform::Dreamcast).recordCompleted("levelb1");
    second.progress().legendsProgress(LegendsPlatform::Dreamcast).shards = 0x1e;
    scene->synchronizeProgress(party);
    CHECK(scene->dreamcastPortalUnlocked(next));
    CHECK(scene->dreamcastPortalUnlocked(temple));
    const Vec3 contact = scene->dreamcastPortals()[mountain].position;
    const u32 loopId = 0x40000000U + scene->dreamcastPortals()[mountain].nativeIndex;
    const auto standingLoops = [&] {
        auto loops = scene->soundLoops();
        std::erase_if(loops, [](const auto& loop) { return (loop.id & 0x40000000U) == 0; });
        return loops;
    };
    CHECK(standingLoops().empty());
    std::array visitors{LegendsPortalVisitor{0, contact},
                        LegendsPortalVisitor{1, contact + Vec3{50, 0, 0}}};
    for (s32 tick = 0; tick < 120; ++tick) {
        scene->update(1.0f / 30);
        CHECK_FALSE(scene->tickPortals(visitors, 1.0f / 30));
    }
    const auto loops = standingLoops();
    REQUIRE(loops.size() == 1);
    CHECK(loops.front().id == loopId);
    CHECK(loops.front().token == 10);
    CHECK(loops.front().position == contact);
    // Native ACTIVE2 has no repeat flag. Its moving mesh must hold its last
    // pose while the flame/texture effects continue and a second player joins.
    s32 figureId = -1;
    for (usize i = 0; i < scene->fixtureCount(); ++i) {
        const auto object =
            scene->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(i));
        if (object && object->visible && object->name == "EXIT_PORTAL" &&
            object->position == contact) {
            figureId = object->id;
            break;
        }
    }
    REQUIRE(figureId >= 0);
    const auto portalVertices = [&] {
        device.draws.clear();
        scene->highlightFixture(device, Mat4{1}, CameraFrame::of(WorldCamera{}), -1, figureId,
                                SelectionHighlight{contact});
        std::vector<Vec3> vertices;
        for (const auto& draw : device.draws)
            for (const auto& vertex : draw.vertices)
                vertices.emplace_back(draw.transform * Vec4{vertex.position, 1});
        return vertices;
    };
    const auto held = portalVertices();
    REQUIRE_FALSE(held.empty());
    for (s32 tick = 0; tick < 10; ++tick) {
        scene->update(1.0f / 30);
        CHECK_FALSE(scene->tickPortals(visitors, 1.0f / 30));
    }
    const auto later = portalVertices();
    REQUIRE(later.size() == held.size());
    for (usize i = 0; i < later.size(); ++i)
        CHECK(glm::distance(later[i], held[i]) < .0001f);
    visitors[1].position = contact;
    std::optional<std::string> destination;
    for (s32 tick = 0; tick < 120 && !destination; ++tick) {
        scene->update(1.0f / 30);
        destination = scene->tickPortals(visitors, 1.0f / 30);
    }
    REQUIRE(destination);
    CHECK(*destination == "levelb1");
    CHECK(standingLoops().empty());
    CHECK_FALSE(scene->tickPortals(visitors, 1));
    scene->cancelPortalTravel();
    CHECK(standingLoops().empty());
    for (s32 tick = 0; tick < 120; ++tick) {
        CHECK_FALSE(scene->tickPortals(visitors, 1.0f / 30));
    }
    for (auto& visitor : visitors)
        visitor.position += Vec3{50, 0, 0};
    CHECK_FALSE(scene->tickPortals(visitors, 1.0f / 30));
    for (auto& visitor : visitors)
        visitor.position = contact;
    destination.reset();
    for (s32 tick = 0; tick < 120 && !destination; ++tick) {
        destination = scene->tickPortals(visitors, 1.0f / 30);
    }
    REQUIRE(destination);
    CHECK(*destination == "levelb1");
}

} // namespace
