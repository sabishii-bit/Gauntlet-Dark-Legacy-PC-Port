#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"

#include "game/world/TowerCamera.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

WorldLocator marker(const Vec3& position, f32 pitch, f32 yaw, u8 distance = 0) {
    WorldLocator out;
    out.kind = LocatorKind::TriggerCamera;
    out.position = position;
    out.rotation = Vec3{pitch, yaw, 0.0f};
    out.delay = distance;
    return out;
}

CameraSubject standing(f32 x, f32 z) {
    return CameraSubject{Vec3{x, 0.0f, z}, Vec3{x, 2.5f, z}};
}

TEST_CASE("the camera settles behind one player at the marker's angle and the shortest range",
          "[game][world][camera]") {
    const std::vector<WorldLocator> markers{marker(Vec3{0.0f, 10.0f, 0.0f}, 0.5f, 0.3f)};
    const std::vector<CameraSubject> party{standing(0.0f, 0.0f)};
    TowerCamera camera;
    camera.reset(party, markers, CameraRange{}, CameraView{});
    REQUIRE(camera.marker() == 0);
    REQUIRE(camera.yaw() == Approx(0.3f)); // the camera looks the way the marker faces
    REQUIRE(camera.pitch() == Approx(0.5f));
    REQUIRE_FALSE(camera.turning());
    REQUIRE(camera.distance() == Approx(24.0f).margin(0.01f));
    REQUIRE(camera.attention().x == Approx(0.0f).margin(0.01f));
    REQUIRE(camera.attention().y == Approx(2.5f).margin(0.01f)); // the body's centre
    const Vec3 back = camera.attention() - camera.camera().forward() * camera.distance();
    REQUIRE(camera.camera().position.x == Approx(back.x));
    REQUIRE(camera.camera().position.y == Approx(back.y));
    REQUIRE(camera.camera().position.z == Approx(back.z));
    REQUIRE(camera.camera().position.y > 2.5f); // looking down from above

    // Settled, a frame changes nothing.
    REQUIRE_FALSE(camera.update(party, markers, CameraRange{}, CameraView{}, 1.0f / 30.0f));
}

TEST_CASE("a spread party pulls the camera out until everyone fits the view",
          "[game][world][camera]") {
    const std::vector<WorldLocator> markers{marker(Vec3{0.0f, 10.0f, 0.0f}, 0.2f, 0.0f)};
    std::vector<CameraSubject> party{standing(-2.0f, 0.0f), standing(2.0f, 0.0f)};
    TowerCamera camera;
    camera.reset(party, markers, CameraRange{}, CameraView{});
    // Two players are looked down on by at least the level's minimum pitch.
    REQUIRE(camera.pitch() == Approx(CameraRange{}.minPitch));
    // Close together they still fit at the shortest range.
    REQUIRE(camera.distance() == Approx(24.0f).margin(0.01f));
    REQUIRE(camera.attention().x == Approx(0.0f).margin(0.01f));

    party[0] = standing(-30.0f, 0.0f);
    party[1] = standing(30.0f, 0.0f);
    for (int i = 0; i < 200; ++i) {
        camera.update(party, markers, CameraRange{}, CameraView{}, 1.0f / 30.0f);
    }
    // Half the spread over the half-angle's tangent, plus the slack the fit adds.
    const f32 needed = 30.0f / std::tan(CameraView{}.horizontalFov * 0.5f);
    REQUIRE(camera.distance() == Approx(needed + TowerCamera::kFarGap).margin(0.5f));

    // A marker with a fixed distance overrides the range for the party.
    const std::vector<WorldLocator> fixed{marker(Vec3{0.0f, 10.0f, 0.0f}, 0.2f, 0.0f, 40)};
    party[0] = standing(-2.0f, 0.0f);
    party[1] = standing(2.0f, 0.0f);
    camera.reset(party, fixed, CameraRange{}, CameraView{});
    REQUIRE(camera.distance() == Approx(24.0f).margin(0.01f)); // compact: the shortest range wins
}

TEST_CASE("a clearly nearer marker takes over and the camera turns to it over fifty steps",
          "[game][world][camera]") {
    const std::vector<WorldLocator> markers{marker(Vec3{0.0f, 0.0f, 0.0f}, 0.3f, kPi),
                                            marker(Vec3{100.0f, 0.0f, 0.0f}, 0.6f, kPi / 2.0f)};
    std::vector<CameraSubject> party{standing(0.0f, 0.0f)};
    TowerCamera camera;
    camera.reset(party, markers, CameraRange{}, CameraView{});
    REQUIRE(camera.marker() == 0);
    REQUIRE(camera.yaw() == Approx(kPi));

    // Half way between the markers the first still holds; well past it the second wins.
    party[0] = standing(55.0f, 0.0f);
    for (int i = 0; i < 300; ++i) {
        camera.update(party, markers, CameraRange{}, CameraView{}, 1.0f / 30.0f);
    }
    REQUIRE(camera.marker() == 0);
    party[0] = standing(90.0f, 0.0f);
    bool turned = false;
    int frames = 0;
    for (; frames < 300 && (camera.marker() != 1 || camera.turning()); ++frames) {
        camera.update(party, markers, CameraRange{}, CameraView{}, 1.0f / 30.0f);
        turned = turned || camera.turning();
    }
    REQUIRE(camera.marker() == 1);
    REQUIRE(turned);
    REQUIRE(frames < 300);
    REQUIRE(camera.yaw() == Approx(kPi / 2.0f).margin(1e-4f));
    REQUIRE(camera.pitch() == Approx(0.6f).margin(1e-4f));

    // Without markers the camera looks straight along +z.
    camera.reset(party, {}, CameraRange{}, CameraView{});
    REQUIRE(camera.marker() == -1);
    REQUIRE(camera.yaw() == 0.0f);
}

} // namespace
