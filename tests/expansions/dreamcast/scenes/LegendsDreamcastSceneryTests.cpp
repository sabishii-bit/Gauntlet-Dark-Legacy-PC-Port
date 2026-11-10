
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

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
