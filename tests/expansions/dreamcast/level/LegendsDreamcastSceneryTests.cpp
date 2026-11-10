#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/dreamcast/level/LegendsDreamcastScenery.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/MissileTarget.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast shoot walls share transformed attack and collision surfaces",
          "[legends][dreamcast-scenery]") {
    const formats::ItemInfoRecord info{.type = 10,
                                       .subtype = 42,
                                       .collisionType = 4,
                                       .name = {},
                                       .armor = 1,
                                       .hitPoints = 25,
                                       .choices = {}};
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
} // namespace
