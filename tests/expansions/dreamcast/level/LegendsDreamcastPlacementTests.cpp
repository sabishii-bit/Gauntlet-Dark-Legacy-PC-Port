#include <cmath>
#include <filesystem>
#include <map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/level/ItemFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

// Independently transcribed column stores at 1ST_READ.BIN 8c0eceb8..8c0ecf20.
// The three native sine values are negated before those stores. This is not
// the camera's differently ordered CreateYPR at 8c0ed040.
Mat4 nativeItemMatrix(Vec3 position, Vec3 angles) {
    const f32 sp = std::sin(angles.x);
    const f32 cp = std::cos(angles.x);
    const f32 sy = std::sin(angles.y);
    const f32 cy = std::cos(angles.y);
    const f32 sr = std::sin(angles.z);
    const f32 cr = std::cos(angles.z);
    return {Vec4{cy * cr, -sp * sy * cr - cp * sr, -cp * sy * cr + sp * sr, 0},
            Vec4{cy * sr, cp * cr - sp * sy * sr, -cp * sy * sr - sp * cr, 0},
            Vec4{sy, sp * cy, cp * cy, 0}, Vec4{position, 1}};
}

void checkMatrix(const Mat4& actual, const Mat4& expected) {
    for (s32 column = 0; column < 4; ++column) {
        CAPTURE(column);
        CHECK(glm::length(actual[column] - expected[column]) < 0.00001f);
    }
}

TEST_CASE("Dreamcast item placement retains native facing and local rotation order",
          "[legends][dreamcast-placement]") {
    const Vec3 origin{3, 7, -11};
    checkMatrix(legendsDreamcastItemPlacement(origin, Vec3{0}), glm::translate(Mat4{1}, origin));
    const auto yaw = legendsDreamcastItemPlacement(origin, {0, kHalfPi, 0});
    CHECK(glm::length(Vec3(yaw * Vec4{0, 0, 1, 0}) - Vec3{1, 0, 0}) < 0.00001f);
    const auto pitch = legendsDreamcastItemPlacement(origin, {kHalfPi, 0, 0});
    CHECK(glm::length(Vec3(pitch * Vec4{0, 0, 1, 0}) - Vec3{0, 1, 0}) < 0.00001f);
    const auto roll = legendsDreamcastItemPlacement(origin, {0, 0, kHalfPi});
    CHECK(glm::length(Vec3(roll * Vec4{1, 0, 0, 0}) - Vec3{0, -1, 0}) < 0.00001f);
    for (const auto angles :
         {Vec3{.3f, .7f, -.4f}, Vec3{-.06654595f, 1.2f, -.0761889f}, Vec3{kPi, -kHalfPi, kPi}}) {
        checkMatrix(legendsDreamcastItemPlacement(origin, angles),
                    nativeItemMatrix(origin, angles));
    }

    // The expansion boundary must not alter the GameCube/N64 convention.
    const auto legacy = itemPlacement(origin, {0, kHalfPi, 0});
    CHECK(glm::length(Vec3(legacy * Vec4{0, 0, 1, 0}) - Vec3{-1, 0, 0}) < 0.00001f);
}

TEST_CASE("Extracted Dreamcast chest placements match native matrix stores at every angle",
          "[legends][dreamcast-placement][assets]") {
    const auto root =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_directory(root)) {
        SKIP("Dreamcast extraction is not installed");
    }
    usize chests = 0;
    usize tilted = 0;
    usize nontrivialYaw = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto path = entry.path() / "worlds.rom";
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        const auto bytes = readFile(path);
        if (bytes.size() < 96) {
            continue;
        }
        const auto scene = formats::LegendsDreamcastScene::parse(bytes);
        for (usize index = 0; index < scene.itemInstances.size(); ++index) {
            const auto& item = scene.itemInstances[index];
            const auto& info = scene.itemInfos[static_cast<usize>(item.info)];
            if (info.type != 2 || info.subtype < 46 || info.subtype > 48) {
                continue;
            }
            CAPTURE(path, index, info.name, item.rotation.x, item.rotation.y, item.rotation.z);
            checkMatrix(legendsDreamcastItemPlacement(item.position, item.rotation),
                        nativeItemMatrix(item.position, item.rotation));
            ++chests;
            tilted +=
                (std::abs(item.rotation.x) > .001f || std::abs(item.rotation.z) > .001f) ? 1U : 0U;
            nontrivialYaw += std::abs(std::sin(item.rotation.y)) > .1f ? 1U : 0U;
        }
    }
    CHECK(chests == 495);
    CHECK(tilted > 0);
    CHECK(nontrivialYaw > 0);
}

TEST_CASE("Dreamcast scene draws repeated chests at their authored native facing",
          "[legends][dreamcast-placement][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Dreamcast extraction is not installed");
    }
    const auto world = readFile(directory / "worlds.rom");
    const auto models = readFile(directory / "objects.rom");
    const auto native = formats::LegendsDreamcastScene::parse(world);
    auto map = formats::LegendsMap::parseDreamcast(world, models);
    map.textures =
        formats::decodeLegendsDreamcastTextures(models, readFile(directory / "textures.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    struct Reference {
        Vec3 angles;
        std::vector<Vec3> vertices;
    };
    std::map<std::string, Reference> reference;
    usize compared = 0;
    for (const auto& item : native.itemInstances) {
        const auto& info = native.itemInfos[static_cast<usize>(item.info)];
        if (info.type != 2 || info.subtype < 46 || info.subtype > 48) {
            continue;
        }
        const auto& name = item.name.empty() ? info.name : item.name;
        for (usize index = 0; index < scene->fixtureCount(); ++index) {
            const auto object = scene->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase +
                                                     static_cast<s32>(index));
            if (!object || !object->visible || object->name != name ||
                glm::distance(Vec2{object->position.x, object->position.z},
                              Vec2{item.position.x, item.position.z}) > .001f) {
                continue;
            }
            device.draws.clear();
            // The selection eye-lift is centred on the item, so it commutes
            // with rotation. Convert its rendered vertices back using the
            // independent native matrix, not the production placement helper.
            scene->highlightFixture(device, Mat4{1}, CameraFrame::of(WorldCamera{}), -1, object->id,
                                    SelectionHighlight{object->position});
            const Mat4 local = glm::inverse(nativeItemMatrix(object->position, item.rotation));
            std::vector<Vec3> vertices;
            for (const auto& draw : device.draws) {
                for (const auto& vertex : draw.vertices) {
                    vertices.emplace_back(local * draw.transform * Vec4{vertex.position, 1});
                }
            }
            REQUIRE_FALSE(vertices.empty());
            const auto [first, inserted] = reference.try_emplace(name, item.rotation, vertices);
            if (!inserted && glm::length(first->second.angles - item.rotation) > .1f) {
                CAPTURE(name, item.position.x, item.position.z);
                REQUIRE(vertices.size() == first->second.vertices.size());
                for (usize v = 0; v < vertices.size(); ++v) {
                    REQUIRE(glm::distance(vertices[v], first->second.vertices[v]) < .001f);
                }
                ++compared;
            }
            break;
        }
    }
    CHECK(compared > 0);
}
} // namespace
