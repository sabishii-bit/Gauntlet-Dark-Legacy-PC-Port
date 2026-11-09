#include <algorithm>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("Dreamcast A4 broken pillar retains its native wall and adjacent key-door route",
          "[legends][dreamcast][dreamcast-a4-traversal][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levela4";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom"))
        SKIP("Optional Dreamcast extraction is not installed");
    const auto bytes = readFile(directory / "worlds.rom");
    const auto native = formats::LegendsDreamcastScene::parse(bytes);
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    REQUIRE(map.objects.size() > 1019);
    REQUIRE(map.objects[365].name == "A4BROKEN_PILAR");
    REQUIRE(map.objects[807].name == "A4GROUP359");
    REQUIRE(map.objects[1019].name == "A4FLOOR#58");
    const Vec3 pillar = Vec3{map.objects[365].transform[3]};
    CHECK(pillar.x == Approx(-51.28f));
    CHECK(pillar.z == Approx(-.73f));

    // Dreamcast shoot walls are native type10/sub42 item records, unlike
    // N64's separate SWAL roster. This decorative broken pillar is neither
    // one of those four records nor an animated/switch-controlled object.
    usize authoredWalls = 0;
    for (const auto& item : native.itemInstances) {
        if (item.info < 0)
            continue;
        const auto& info = native.itemInfos.at(static_cast<usize>(item.info));
        if (info.type != 10 || info.subtype != 42)
            continue;
        ++authoredWalls;
        CHECK(item.name.starts_with("A4SHOOTWALL#"));
        CHECK(glm::distance(Vec2{item.position.x, item.position.z}, Vec2{pillar.x, pillar.z}) >
              50.f);
    }
    REQUIRE(authoredWalls == 4);

    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(scene);
    for (const auto& warning : scene->warnings())
        INFO(warning);
    REQUIRE_FALSE(scene->strikeWorldObject(365, 10000, 0));
    CHECK(scene->worldObjectVisible(365));

    SECTION("the four authored shoot walls remain attackable and remove their collision") {
        const auto targets = scene->sceneryTargets();
        REQUIRE(targets.size() == authoredWalls);
        WorldCollision collision;
        scene->bindCollision(collision);
        for (const auto& target : targets) {
            REQUIRE_FALSE(target.surface.empty());
            const s32 owner = target.surface.front().object;
            const auto figure = scene->fixtureObject(owner);
            REQUIRE(figure);
            REQUIRE(figure->name.starts_with("A4SHOOTWALL#"));
            REQUIRE(figure->visible);
            CHECK(collision.solid(owner));
            REQUIRE(scene->strikeWorldObject(owner, 10000, 0));
            scene->syncCollision(collision);
            CHECK_FALSE(collision.solid(owner));
            REQUIRE(scene->fixtureObject(owner));
            CHECK_FALSE(scene->fixtureObject(owner)->visible);
        }
        CHECK(scene->sceneryTargets().empty());
        CHECK(scene->worldObjectVisible(365));
    }

    SECTION("a player opens gate244 and walks around rather than through the pillar") {
        const auto& gate = native.itemInstances.at(244);
        const auto& info = native.itemInfos.at(static_cast<usize>(gate.info));
        REQUIRE(info.type == 7);
        REQUIRE(info.name == "GATEB");
        REQUIRE(gate.position.x == Approx(-45.5949f));
        REQUIRE(gate.position.z == Approx(-5.6826f));
        s32 gateObject = -1;
        for (usize i = 0; i < scene->fixtureCount(); ++i) {
            const s32 id = LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(i);
            const auto object = scene->fixtureObject(id);
            if (object && object->name == info.name &&
                glm::distance(Vec2{object->position.x, object->position.z},
                              Vec2{gate.position.x, gate.position.z}) < .001f) {
                gateObject = id;
                break;
            }
        }
        REQUIRE(gateObject >= 0);
        const Vec3 gatePosition = scene->fixtureObject(gateObject)->position;
        const auto closed = [&] {
            return std::ranges::any_of(scene->containerObstacles(), [&](const auto& obstacle) {
                return glm::distance(obstacle.centre, gatePosition) < .002f;
            });
        };
        REQUIRE(closed());
        CharacterSave save;
        save.character = 0;
        save.progress().inventory.keys = 0;
        // A4FLOOR#58 is the actual floor on the north side of the door.
        map.starts = {{{gate.position.x, .04f, -.5f}, kPi}};
        LegendsTraversal traversal;
        traversal.open(map, 0, save);
        REQUIRE(traversal.authoredStart());
        REQUIRE(traversal.actor().floorObject() == 1019);
        REQUIRE(traversal.collision().sweepSphere({-55, 1.5f, 0}, {-48, 1.5f, 0}, .1f));
        const auto walk = [&](s32 ticks) {
            for (s32 tick = 0; tick < ticks; ++tick) {
                const PlayerActor before = traversal.actor();
                traversal.step({{0, -1}, 1}, 0, 1.f / 60);
                const Vec3 corrected =
                    scene->resolveMovement(before.position(), traversal.actor().position(),
                                           traversal.actor().radius(), traversal.actor().height());
                (void)traversal.correctSceneryStep(before, corrected, 1.f / 60);
                // Ordinary imported gates enter LevelFixtures::update through
                // obstacles(), not the separate tower/scenery correction above.
                // Use its same push-out plus swept actor.slide before contact.
                for (const auto& obstacle : scene->containerObstacles()) {
                    auto& actor = traversal.actor();
                    const Vec3 position = actor.position();
                    actor.slide(obstacle.pushOut(position, actor.radius()) - position,
                                &traversal.collision());
                }
                (void)scene->collect(traversal.actor(), 60, false);
                scene->update(1.f / 60);
                REQUIRE(std::isfinite(traversal.actor().position().y));
            }
        };
        walk(180);
        CAPTURE(traversal.actor().position().x, traversal.actor().position().y,
                traversal.actor().position().z);
        CHECK(closed());
        CHECK(traversal.actor().position().z > gate.position.z);
        traversal.actor().save().progress().inventory.keys = 1;
        walk(240);
        CAPTURE(traversal.actor().position().x, traversal.actor().position().y,
                traversal.actor().position().z);
        CHECK_FALSE(closed());
        CHECK(traversal.actor().save().progress().inventory.keys == 0);
        CHECK(traversal.actor().position().z < gate.position.z - 4);
        CHECK(traversal.actor().position().y == Approx(.04f).margin(.02f));
        CHECK(scene->worldObjectVisible(365));
    }
}

} // namespace
