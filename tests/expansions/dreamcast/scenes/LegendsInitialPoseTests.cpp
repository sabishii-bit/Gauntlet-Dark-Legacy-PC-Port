#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "expansions/dreamcast/formats/LegendsSceneMotion.h"
#include "expansions/dreamcast/scenes/LegendsInitialPose.h"

namespace {
using namespace gdl;
using namespace gdl::formats;
using namespace gdl::game;

LegendsSceneMotion motion() {
    LegendsSceneMotion result;
    result.objects.resize(3);
    result.parents = {-1, 0, -1};
    result.objects[0].flags = 0x103c;
    result.objects[0].position = {10, 0, 0};
    result.objects[1].flags = 0x103c;
    result.objects[1].position = {2, 0, 0};
    result.objects[2].flags = 0x3c;
    result.objects[2].position = {50, 0, 0};
    WorldAnimationRecord track;
    track.objectIndex = 0;
    track.frameCount = 2;
    track.track.flags = NodeTrack::kRotationZ | NodeTrack::kPositionY;
    track.track.frames = {0, 1};
    track.track.values = {kHalfPi, 3.0f, 0.0f, 8.0f};
    result.worldAnimations.push_back(std::move(track));
    return result;
}

LegendsMap map() {
    LegendsMap result;
    result.objects.resize(3);
    result.objects[0].transform = glm::translate(Mat4{1}, Vec3{10, 0, 0});
    result.objects[1].transform = glm::translate(Mat4{1}, Vec3{12, 0, 0});
    result.objects[2].transform = glm::translate(Mat4{1}, Vec3{50, 0, 0});
    LegendsMapCollisionTriangle triangle;
    triangle.vertices = {Vec3{12, 0, 0}, Vec3{13, 0, 0}, Vec3{12, 0, 1}};
    triangle.owner = 1;
    triangle.flags = 0x103c;
    triangle.floor = true;
    triangle.wall = true;
    result.collision.push_back(triangle);
    triangle.owner = 2;
    triangle.flags = 0x3c;
    result.collision.push_back(triangle);
    return result;
}

void close(const Vec3& actual, const Vec3& expected) {
    CHECK(glm::length(actual - expected) < 0.00001f);
}
} // namespace

TEST_CASE("Dreamcast initial world pose moves its hierarchy and only dynamic collision",
          "[legends][initial-pose]") {
    auto scene = map();
    const auto staticCollision = scene.collision[1];
    applyDreamcastInitialPose(scene, motion());
    close(Vec3(scene.objects[0].transform[3]), {10, 3, 0});
    close(Vec3(scene.objects[1].transform[3]), {10, 1, 0});
    close(scene.collision[0].vertices[0], {10, 1, 0});
    close(scene.collision[0].vertices[1], {10, 0, 0});
    close(scene.collision[0].normal, {1, 0, 0});
    CHECK_FALSE(scene.collision[0].floor);
    CHECK(scene.collision[0].wall);
    CHECK(scene.collision[1].vertices == staticCollision.vertices);
    CHECK(scene.collision[1].normal == staticCollision.normal);
    const auto prior = scene;
    applyDreamcastInitialPose(scene, motion());
    CHECK(scene.objects[1].transform == prior.objects[1].transform);
    CHECK(scene.collision[0].vertices == prior.collision[0].vertices);
    CHECK(scene.collision[0].normal == prior.collision[0].normal);
}

TEST_CASE("Dreamcast initial track uses its authored start and native disable flags",
          "[legends][initial-pose]") {
    auto scene = map();
    auto data = motion();
    SECTION("reverse does not force its final frame") {
        data.objects[0].flags |= 0x100000;
        applyDreamcastInitialPose(scene, data);
        close(Vec3(scene.objects[0].transform[3]), {10, 3, 0});
    }
    SECTION("authored final frame is sampled without advancing") {
        data.worldAnimations[0].startFrame = 1;
        applyDreamcastInitialPose(scene, data);
        close(Vec3(scene.objects[0].transform[3]), {10, 8, 0});
        close(scene.collision[0].normal, {0, 1, 0});
    }
    SECTION("both flags leave the rest pose") {
        data.objects[0].flags |= 0x300000;
        applyDreamcastInitialPose(scene, data);
        close(Vec3(scene.objects[0].transform[3]), {10, 0, 0});
        close(Vec3(scene.objects[1].transform[3]), {12, 0, 0});
    }
    SECTION("a disabled parent does not gain collision") {
        data.objects[0].noCollision = true;
        applyDreamcastInitialPose(scene, data);
        CHECK_FALSE(scene.collision[0].floor);
        CHECK_FALSE(scene.collision[0].wall);
    }
}

TEST_CASE("Dreamcast initial pose rejects malformed ownership without partial changes",
          "[legends][initial-pose]") {
    auto scene = map();
    auto data = motion();
    SECTION("different object roster") {
        data.objects.pop_back();
    }
    SECTION("invalid animation target") {
        data.worldAnimations[0].objectIndex = 99;
    }
    SECTION("cyclic hierarchy") {
        data.parents[0] = 1;
    }
    SECTION("invalid parent") {
        data.parents[0] = 99;
    }
    SECTION("invalid collision owner") {
        scene.collision[1].owner = 99;
    }
    const auto before = scene;
    CHECK_THROWS_AS(applyDreamcastInitialPose(scene, data), FormatError);
    CHECK(scene.objects[0].transform == before.objects[0].transform);
    CHECK(scene.collision[0].vertices == before.collision[0].vertices);
}

TEST_CASE("Every native Dreamcast map applies initial keys with finite collision",
          "[legends][initial-pose][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::exists(root)) {
        SKIP("Native Dreamcast extraction is not installed");
    }
    usize maps = 0;
    usize changed = 0;
    for (const auto& folder : std::filesystem::directory_iterator(root / "levels")) {
        const auto path = folder.path() / "worlds.rom";
        if (!folder.is_directory() || !std::filesystem::exists(path) ||
            std::filesystem::file_size(path) == 0) {
            continue;
        }
        INFO(folder.path().string());
        const auto worlds = readFile(path);
        const auto data = LegendsSceneMotion::parseDreamcast(worlds);
        auto scene = LegendsMap::parseDreamcast(worlds, readFile(folder.path() / "objects.rom"));
        std::vector<Mat4> before;
        before.reserve(scene.objects.size());
        for (const auto& object : scene.objects) {
            before.push_back(object.transform);
        }
        applyDreamcastInitialPose(scene, data);
        for (usize i = 0; i < before.size(); ++i) {
            changed += before[i] != scene.objects[i].transform ? 1U : 0U;
        }
        for (const auto& triangle : scene.collision) {
            CHECK(std::isfinite(triangle.normal.x));
            CHECK(std::isfinite(triangle.normal.y));
            CHECK(std::isfinite(triangle.normal.z));
            CHECK(std::abs(glm::length(triangle.normal) - 1.0f) < 0.0001f);
            for (const auto& vertex : triangle.vertices) {
                CHECK(std::isfinite(vertex.x));
                CHECK(std::isfinite(vertex.y));
                CHECK(std::isfinite(vertex.z));
            }
        }
        const auto collision = scene.collision;
        applyDreamcastInitialPose(scene, data);
        REQUIRE(scene.collision.size() == collision.size());
        for (usize i = 0; i < collision.size(); ++i) {
            REQUIRE(scene.collision[i].vertices == collision[i].vertices);
            REQUIRE(scene.collision[i].normal == collision[i].normal);
        }
        ++maps;
    }
    CHECK(maps == 41);
    CHECK(changed > 0);
}
