#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/world/CameraMovementLimit.h"

namespace {
using namespace gdl;
using namespace gdl::game;

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
} // namespace
