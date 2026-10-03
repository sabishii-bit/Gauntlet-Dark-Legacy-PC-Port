#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/world/CameraMovementLimit.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("camera boundaries retain tangential steps and permit returning into view",
          "[game][camera-limit]") {
    WorldCamera camera;
    camera.position = Vec3{0, 15, -25};
    camera.pitch = std::atan2(15.0f, 25.0f);
    const CameraView projection;
    const Vec3 before{30, 0, 0};
    const Vec3 after = before + Vec3{1, 0, 1};
    const Vec3 result = CameraMovementLimit::constrain(before, after, Vec3{0}, camera, projection);
    CHECK(glm::distance(result, before) > 0.1f);
    CHECK(glm::distance(result, after) > 0.1f);
    const Vec3 normal = camera.right() - (1.0f - 60.0f / 640.0f) *
                                             std::tan(projection.horizontalFov / 2) *
                                             camera.forward();
    CHECK(glm::dot(result - before, normal) == Approx(0).margin(1.0e-5));
    CHECK(CameraMovementLimit::constrain(before, before - Vec3{1, 0, 0}, Vec3{0}, camera,
                                         projection) == before - Vec3{1, 0, 0});
    CHECK(CameraMovementLimit::constrain(Vec3{0}, Vec3{1, 0, 1}, Vec3{0}, camera, projection) ==
          Vec3{1, 0, 1});
    const Vec3 elevated =
        CameraMovementLimit::constrain(before, after + Vec3{0, 1, 0}, Vec3{0}, camera, projection);
    CHECK(elevated.y == 1);
}

TEST_CASE("camera boundaries allow recovery and block further separation", "[game][camera-limit]") {
    WorldCamera camera;
    camera.position = Vec3{0, 15, -25};
    camera.pitch = std::atan2(15.0f, 25.0f);
    const CameraView projection;
    const Vec3 attention{0};
    REQUIRE(CameraMovementLimit::allows(Vec3{0}, Vec3{1, 0, 0}, attention, camera, projection));
    REQUIRE_FALSE(
        CameraMovementLimit::allows(Vec3{30, 0, 0}, Vec3{31, 0, 0}, attention, camera, projection));
    REQUIRE(
        CameraMovementLimit::allows(Vec3{30, 0, 0}, Vec3{29, 0, 0}, attention, camera, projection));
    REQUIRE_FALSE(CameraMovementLimit::allows(Vec3{-30, 0, 0}, Vec3{-31, 0, 0}, attention, camera,
                                              projection));
    REQUIRE(CameraMovementLimit::allows(Vec3{-30, 0, 0}, Vec3{-29, 0, 0}, attention, camera,
                                        projection));
    REQUIRE_FALSE(CameraMovementLimit::allows(Vec3{0, 0, 100}, Vec3{0, 0, 101}, attention, camera,
                                              projection));
    REQUIRE(CameraMovementLimit::allows(Vec3{0, 0, 100}, Vec3{0, 0, 99}, attention, camera,
                                        projection));
    REQUIRE_FALSE(CameraMovementLimit::allows(Vec3{0, 0, -100}, Vec3{0, 0, -101}, attention, camera,
                                              projection));
    REQUIRE(CameraMovementLimit::allows(Vec3{0, 0, -100}, Vec3{0, 0, -99}, attention, camera,
                                        projection));
}

TEST_CASE("a corner projection cannot restore movement through an already limited edge",
          "[game][camera-limit]") {
    WorldCamera camera;
    camera.position = Vec3{0, 15, -25};
    camera.pitch = std::atan2(15.0f, 25.0f);
    const CameraView projection;
    const Vec3 before{50, 0, 60};
    // Sliding along the right edge adds forward motion. Removing that motion at
    // the top edge must not leave a rightward escape through the first edge.
    const Vec3 after = before + Vec3{1, 0, 0};
    CHECK(CameraMovementLimit::constrain(before, after, Vec3{0}, camera, projection) == before);
    const Vec3 returning = before - Vec3{1, 0, 1};
    CHECK(CameraMovementLimit::constrain(before, returning, Vec3{0}, camera, projection) ==
          returning);
}

TEST_CASE("opposing players cannot zoom the shared camera out without bound",
          "[game][camera-limit]") {
    std::array<CameraSubject, 2> party{
        {{Vec3{-2, 0, 0}, Vec3{-2, 2.5f, 0}}, {Vec3{2, 0, 0}, Vec3{2, 2.5f, 0}}}};
    TowerCamera camera;
    const CameraRange range;
    const CameraView view;
    camera.reset(party, {}, range, view);
    for (s32 frame = 0; frame < 3000; ++frame) {
        for (usize i = 0; i < party.size(); ++i) {
            const Vec3 step{i == 0 ? -0.2f : 0.2f, 0, 0};
            if (CameraMovementLimit::allows(party[i].feet, party[i].feet + step, camera.attention(),
                                            camera.camera(), view)) {
                party[i].feet += step;
                party[i].follow += step;
            }
        }
        camera.update(party, {}, range, view, 1.0f / 30.0f);
    }
    REQUIRE(camera.distance() <= range.radiusMax + 0.01f);
    REQUIRE(party[1].feet.x - party[0].feet.x < 40);
    REQUIRE(party[1].feet.x - party[0].feet.x > 10);
    REQUIRE(CameraMovementLimit::allows(party[0].feet, party[0].feet + Vec3{0.2f, 0, 0},
                                        camera.attention(), camera.camera(), view));
}
TEST_CASE("four opposing players stay within the shared view and can walk back together",
          "[game][camera-limit][multiplayer]") {
    const std::array<Vec3, 4> directions{Vec3{-1, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, -1},
                                         Vec3{0, 0, 1}};
    std::array<CameraSubject, 4> party;
    for (usize i = 0; i < party.size(); ++i) {
        party[i] = {directions[i] * 2.0f, directions[i] * 2.0f + Vec3{0, 2.5f, 0}};
    }
    TowerCamera camera;
    const CameraRange range;
    const CameraView view;
    camera.reset(party, {}, range, view);
    for (s32 frame = 0; frame < 1800; ++frame) {
        for (usize i = 0; i < party.size(); ++i) {
            const Vec3 after = CameraMovementLimit::constrain(
                party[i].feet, party[i].feet + directions[i] * 0.2f, camera.attention(),
                camera.camera(), view, party[i].follow - party[i].feet);
            party[i].follow += after - party[i].feet;
            party[i].feet = after;
        }
        camera.update(party, {}, range, view, 1.0f / 30.0f);
    }
    CHECK(camera.distance() <= range.radiusMax + 0.01f);
    for (const auto& player : party) {
        // The shared view may translate while edge-tangent motion is preserved.
        // Bound separation from the party, not travel from the world's origin.
        CHECK(glm::distance(player.feet, camera.attention()) < range.radiusMax * 2.0f);
        for (const Vec3& point : {player.feet, player.follow}) {
            const Vec3 relative = point - camera.camera().position;
            const f32 depth = glm::dot(relative, camera.camera().forward());
            REQUIRE(depth > 0);
            const f32 halfWidth = depth * std::tan(view.horizontalFov * 0.5f);
            CHECK(std::abs(glm::dot(relative, camera.camera().right())) < halfWidth);
            CHECK(std::abs(glm::dot(relative, camera.camera().up())) < halfWidth / view.aspect);
        }
        const Vec3 towards = camera.attention() - player.feet;
        const Vec3 back = player.feet + glm::normalize(Vec3{towards.x, 0, towards.z}) * 0.2f;
        const Vec3 recovered =
            CameraMovementLimit::constrain(player.feet, back, camera.attention(), camera.camera(),
                                           view, player.follow - player.feet);
        CHECK(glm::distance(recovered, back) == Approx(0).margin(1.0e-5));
    }
}

} // namespace
