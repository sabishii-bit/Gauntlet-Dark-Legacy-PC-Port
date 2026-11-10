#include <algorithm>
#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A native Dreamcast level's map with its decoded textures. */
formats::LegendsMap nativeMap(const std::filesystem::path& directory, LegendsPlatform platform) {
    REQUIRE(platform == LegendsPlatform::Dreamcast);
    const auto models = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), models);
    map.textures =
        formats::decodeLegendsDreamcastTextures(models, readFile(directory / "textures.rom"));
    return map;
}

TEST_CASE("Dreamcast tower hosts Sumner interaction and restores its own party progress",
          "[legends][dreamcast-tower][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelt1";
    if (!std::filesystem::exists(directory)) {
        SKIP("Dreamcast extraction is not installed");
    }
    auto map = nativeMap(directory, LegendsPlatform::Dreamcast);
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(scene->isTower());
    const auto index = [&](std::string_view name) {
        const auto found = std::ranges::find(map.objects, name, &formats::LegendsMapObject::name);
        REQUIRE(found != map.objects.end());
        return static_cast<usize>(found - map.objects.begin());
    };
    const auto castleGate = index("T1XSNS_DRFIE#0");
    const auto castleBarrier = index("T1TRAPW02");
    const auto desertGate = index("T1XSNS_DRFIE#2");
    const auto rune = index("T1RUNEB1");
    const auto shard = index("T1SHARD02");
    CharacterSave save;
    save.name = "TEST";
    auto& n64 = save.progress().legendsProgress(LegendsPlatform::N64);
    n64.obelisks = n64.runestones = n64.shards = 0xffff;
    scene->synchronizeProgress(std::array<const CharacterSave*, 1>{&save});
    CHECK(scene->worldObjectVisible(castleGate));
    CHECK_FALSE(scene->worldObjectVisible(rune));
    CHECK_FALSE(scene->worldObjectVisible(shard));
    auto& progress = save.progress().legendsProgress(LegendsPlatform::Dreamcast);
    progress.obelisks = 0x38;
    progress.runestones = 8;
    progress.shards = 4;
    scene->synchronizeProgress(std::array<const CharacterSave*, 1>{&save});
    CHECK_FALSE(scene->worldObjectVisible(castleGate));
    CHECK_FALSE(scene->worldObjectVisible(castleBarrier));
    CHECK(scene->worldObjectVisible(desertGate));
    CHECK(scene->worldObjectVisible(rune));
    CHECK(scene->worldObjectVisible(shard));
    WorldCollision collision;
    std::vector<CollisionTriangle> triangles;
    for (const auto& source : map.collision) {
        if (source.floor || source.wall) {
            triangles.push_back({source.normal, source.vertices, source.owner,
                                 (source.floor ? 4U : 0U) | (source.wall ? 2U : 0U)});
        }
    }
    collision.build(std::move(triangles));
    scene->bindCollision(collision);
    CHECK_FALSE(collision.solid(static_cast<s32>(castleGate)));
    CHECK_FALSE(collision.solid(static_cast<s32>(castleBarrier)));
    CHECK(collision.solid(static_cast<s32>(desertGate)));

    LegendsLevel level{device, map, std::move(scene), directory, test::dataDirectory()};
    level.bindCollision(collision);
    PlayerActor player;
    player.spawn(2, save, nullptr, Vec3{0, 0, 9.75f}, 0);
    std::array<PlayerActor*, 1> party{&player};
    level.updateParty(party, 1.0f / 30, false);
    CHECK_FALSE(level.takeDialogue());
    level.interactionButtons(1U << 1); // A different seat's button does not admit this visitor.
    level.updateParty(party, 1.0f / 30, false);
    CHECK_FALSE(level.takeDialogue());
    level.interactionButtons(1U << 2);
    level.updateParty(party, 1.0f / 30, false);
    const auto dialogue = level.takeDialogue();
    REQUIRE(dialogue);
    CHECK(dialogue->player == 2);
    REQUIRE(dialogue->topics.size() == 5);
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    CHECK(dialogue->title.empty());
    for (const auto& topic : dialogue->topics) {
        CHECK(text.has(topic.label));
        const auto pages = level.answerDialogue(topic.code);
        if (topic.code == 4) {
            CHECK(text.get(topic.label) == "exit");
            CHECK(pages.empty());
            continue;
        }
        REQUIRE_FALSE(pages.empty());
        for (const auto& page : pages) {
            CHECK(text.has(page));
            CHECK_FALSE(text.get(page).empty());
        }
    }
    CHECK(level.answerDialogue(0).front() == "legends.dreamcast.sumner.history.5");
    level.dismissDialogue();
    CHECK(level.answerDialogue(0).empty());
    for (int tick = 0; tick < 60; ++tick) {
        level.updateParty(party, 1.0f / 30, false);
        CHECK_FALSE(level.takeDialogue());
    }
    player.place({0, 0, 0});
    level.updateParty(party, 1.0f / 30, false);
    player.place({0, 0, 9.75f});
    level.updateParty(party, 1.0f / 30, false);
    CHECK_FALSE(level.takeDialogue());
    level.interactionButtons(1U << 2);
    level.updateParty(party, 1.0f / 30, false);
    REQUIRE(level.takeDialogue());

    // Native tower framing is independent of the collision ray used by GDL.
    // Check the complete imported floor corpus, including rendered samples.
    const Vec3 towerHub = level.adapter().dreamcastCameraMarkers().front().position;
    LegendsDreamcastTowerCamera expectedCamera;
    std::vector<Vec3> cameraFeet;
    cameraFeet.reserve(map.starts.size() + map.collision.size());
    for (const auto& start : map.starts) {
        cameraFeet.push_back(start.position);
    }
    // Walk the tower's actual floor triangles rather than only its entrance.
    // These samples cover all realm alcoves and their surrounding walls.
    for (const auto& surface : map.collision) {
        if (surface.floor && surface.normal.y >= WorldCollision::kFloorNormalY) {
            cameraFeet.push_back((surface.vertices[0] + surface.vertices[1] + surface.vertices[2]) /
                                 3.0f);
        }
    }
    REQUIRE(cameraFeet.size() > 20);
    for (const Vec3 feet : cameraFeet) {
        const std::array subjects{CameraSubject{feet, feet + Vec3{0, 3, 0}, 3}};
        level.resetCamera(subjects);
        expectedCamera.reset(subjects, towerHub);
        CHECK(*level.cameraAttention() == expectedCamera.attention());
        CHECK(*level.camera() == expectedCamera.camera());
        for (const f32 alpha : {0.0f, .5f, 1.0f}) {
            CHECK(*level.camera(alpha) == expectedCamera.sample(alpha / 60));
        }
    }
}
} // namespace
