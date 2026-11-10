#include <algorithm>
#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/level/LegendsDreamcastWorldMotion.h"
#include "expansions/dreamcast/scenes/LegendsInitialPose.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/level/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast B1 grounded containers and RUNEB1 stay on floors through LevelWorld updates",
          "[legends][dreamcast-motion][dreamcast-placement][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::exists(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
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
        if (figure && figure->name == "RUNEB1") {
            runeFigure = id;
        }
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
        if (tick == 30) {
            CHECK(rune->position.y > before.y + .1f);
        }
    }
    CHECK(rune->position.y > before.y + 1.f);
}
} // namespace
