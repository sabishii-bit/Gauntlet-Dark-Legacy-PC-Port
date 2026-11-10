#include <algorithm>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/SelectionHighlight.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast native gates spend one key and animate without losing console identity",
          "[legends][dreamcast-gates][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Dreamcast extraction is not installed");
    }
    const auto worldBytes = readFile(directory / "worlds.rom");
    const auto models = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(worldBytes, models);
    map.textures =
        formats::decodeLegendsDreamcastTextures(models, readFile(directory / "textures.rom"));
    const auto native = formats::LegendsDreamcastScene::parse(worldBytes);
    const auto placed = std::ranges::find_if(native.itemInstances, [&](const auto& item) {
        return item.info >= 0 && native.itemInfos[static_cast<usize>(item.info)].type == 7;
    });
    REQUIRE(placed != native.itemInstances.end());
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    for (const auto& warning : scene->warnings()) {
        INFO(warning);
    }
    s32 identity = -1;
    for (usize index = 0; index < scene->fixtureCount(); ++index) {
        const s32 id = LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(index);
        const auto object = scene->fixtureObject(id);
        if (object && object->name == "GATEA" &&
            glm::distance(Vec2{object->position.x, object->position.z},
                          Vec2{placed->position.x, placed->position.z}) < .001f) {
            identity = id;
            break;
        }
    }
    REQUIRE(identity >= 0);
    const Vec3 grounded = scene->fixtureObject(identity)->position;
    const auto floor = scene->dreamcastWorldMotion()->supportAt(placed->position);
    REQUIRE(floor);
    CHECK(std::abs(grounded.y - (floor->y + .1f)) < .002f);
    const auto doorObstacle = [&] {
        const auto obstacles = scene->containerObstacles();
        return std::ranges::any_of(obstacles, [&](const auto& body) {
            return glm::distance(body.centre, grounded) < .002f && body.halfAcross == 6 &&
                   body.halfAlong == 1;
        });
    };
    REQUIRE(doorObstacle());
    const CameraFrame camera;
    const SelectionHighlight highlight{grounded + Vec3{0, 10, 20}};
    const auto vertices = [&] {
        device.draws.clear();
        scene->highlightFixture(device, Mat4{1}, camera, -1, identity, highlight);
        std::vector<Vec3> result;
        for (const auto& draw : device.draws) {
            for (const auto& vertex : draw.vertices) {
                result.emplace_back(draw.transform * Vec4{vertex.position, 1});
            }
        }
        return result;
    };
    const auto closed = vertices();
    REQUIRE_FALSE(closed.empty());
    CharacterSave save;
    save.character = 0;
    save.progress().inventory.keys = 0;
    PlayerActor player;
    player.spawn(0, save, nullptr, grounded + Vec3{0, 0, 1.5f}, 0);
    (void)scene->collect(player, 60, false);
    CHECK(doorObstacle());
    player.save().progress().inventory.keys = 2;
    (void)scene->collect(player, 60, false);
    CHECK(player.save().progress().inventory.keys == 1);
    CHECK_FALSE(doorObstacle());
    scene->update(.5f);
    const auto opening = vertices();
    REQUIRE_FALSE(opening.empty());
    CHECK(opening != closed);
    scene->update(3);
    const auto opened = vertices();
    REQUIRE_FALSE(opened.empty());
    CHECK(opened != opening);
    scene->update(3);
    CHECK(vertices() == opened);
    CHECK(scene->fixtureObject(identity)->visible);
    (void)scene->collect(player, 60, false);
    CHECK(player.save().progress().inventory.keys == 1);
    const auto sounds = scene->takeSounds();
    REQUIRE(sounds.size() == 1);
    CHECK(sounds.front().token == 0x210023U); // MOUNT's native S_GATEB1, not an N64 macro.
    CHECK(scene->takeSounds().empty());
}
} // namespace
