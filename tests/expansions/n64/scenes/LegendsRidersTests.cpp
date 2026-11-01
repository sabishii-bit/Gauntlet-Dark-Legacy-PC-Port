#include <algorithm>
#include <array>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;
std::vector<Vec3> nearbyGeometry(LegendsSceneAdapter& scene, test::FakeRenderDevice& device,
                                 const Vec3& position) {
    device.draws.clear();
    const auto frame = CameraFrame::of(WorldCamera{});
    scene.draw(device, Mat4{1}, frame, TreeModel::Pass::Opaque);
    scene.draw(device, Mat4{1}, frame, TreeModel::Pass::Blended);
    std::vector<Vec3> result;
    for (const auto& draw : device.draws) {
        if (draw.vertices.empty()) {
            continue;
        }
        Vec3 centre{0};
        for (const auto& vertex : draw.vertices) {
            centre += Vec3{draw.transform * Vec4{vertex.position, 1}};
        }
        centre /= static_cast<f32>(draw.vertices.size());
        if (glm::length(Vec2{centre.x - position.x, centre.z - position.z}) < 2 &&
            std::abs(centre.y - position.y) < 10) {
            for (const auto& vertex : draw.vertices) {
                result.emplace_back(draw.transform * Vec4{vertex.position, 1});
            }
        }
    }
    return result;
}
} // namespace

TEST_CASE("Native Castle runestone art and pickup contact ride their actual moving floor",
          "[legends][riders][scene-adapter][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = root / "levels/castle3";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("N64 Legends extraction is not installed");
    }
    const auto objects = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects);
    const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                             ? readFile(directory / "texdyn.rom")
                             : std::vector<u8>{};
    for (auto& texture : formats::decodeLegendsN64Textures(objects, dynamic)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    const auto definitions = formats::decodeLegendsN64Switches(
        readFile(directory / "ipr.rom"), readFile(root / "items/generic/ptrap.rom"));
    const LegendsWorldMotion support{map, definitions.switches, 4};
    const auto plate = std::ranges::find(definitions.switches, std::string{"TRAPW16"},
                                         &formats::LegendsN64Switch::target);
    REQUIRE(plate != definitions.switches.end());
    REQUIRE(plate->supported);
    REQUIRE(plate->oneShot());
    REQUIRE(support.platePosition(plate->nativeIndex));
    test::FakeRenderDevice movingDevice;
    test::FakeRenderDevice stillDevice;
    auto movingMap = map;
    auto moving =
        LegendsSceneAdapter::load(movingDevice, LegendsPlatform::N64, directory, movingMap, 4);
    auto still = LegendsSceneAdapter::load(stillDevice, LegendsPlatform::N64, directory, map, 4);
    const auto runestone = std::ranges::find_if(moving->pickups(), [](const auto& pickup) {
        return pickup.nativeIndex == 41 && pickup.award &&
               pickup.award->kind == LegendsRelic::Runestone && pickup.award->index == 2;
    });
    REQUIRE(runestone != moving->pickups().end());
    const usize index = static_cast<usize>(runestone - moving->pickups().begin());
    const Vec3 baseline = runestone->position;
    moving->touchSwitches(std::array{
        LegendsPortalVisitor{0, *support.platePosition(plate->nativeIndex), true, .1f, 3}});
    for (s32 i = 0; i < 60; ++i) {
        moving->capturePresentation();
        still->capturePresentation();
        moving->update(1.0f / 60);
        still->update(1.0f / 60);
    }
    const Vec3 displaced = moving->pickups()[index].position;
    CHECK(displaced.y == Catch::Approx(baseline.y + 4).margin(.001f));
    CHECK(displaced.x == baseline.x);
    CHECK(displaced.z == baseline.z);
    // Pickup contact takes a centre and vertical half-extent, not feet and
    // full player height. The old centre still overlaps a tall pickup after
    // four units of travel, so probe just inside its original lower boundary.
    constexpr f32 kHalfHeight = 1.5f;
    const Vec3 contact = baseline - Vec3{0, still->pickups()[index].height + kHalfHeight - .25f, 0};
    REQUIRE(touchesLegendsPickup(still->pickups()[index], contact, .5f, kHalfHeight));
    CHECK_FALSE(touchesLegendsPickup(moving->pickups()[index], contact, .5f, kHalfHeight));
    CHECK(touchesLegendsPickup(moving->pickups()[index], contact + displaced - baseline, .5f,
                               kHalfHeight));
    const auto movedArt = nearbyGeometry(*moving, movingDevice, displaced);
    const auto stillArt = nearbyGeometry(*still, stillDevice, baseline);
    REQUIRE_FALSE(stillArt.empty());
    REQUIRE(movedArt.size() == stillArt.size());
    for (usize i = 0; i < movedArt.size(); ++i) {
        CHECK(glm::distance(movedArt[i], stillArt[i] + Vec3{0, 4, 0}) < .002f);
    }
    // A second synchronization at unchanged owner displacement must not add it again.
    const f32 y = moving->pickups()[index].position.y;
    moving->update(0);
    CHECK(moving->pickups()[index].position.y == y);
}
