#include <cmath>
#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/world/BossCamera.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr float kPi = std::numbers::pi_v<float>;

/** The town's record: the lich's crypt. */
BossCameraInfo cryptRecord() {
    BossCameraInfo record;
    record.flags = 3;
    record.maxYaw = kPi;
    record.cosMaxYaw = 0.70710677f;
    record.minDistance = 25.0f;
    record.minPlayerDistance = 25.0f;
    record.maxDistance = 75.0f;
    record.maxPlayerDistance = 30.0f;
    record.minPitch = 0.31415927f;
    record.maxPitch = 0.43633232f;
    record.minAttention = Vec3{0.0f, 2.3f, -2.0f};
    record.maxAttention = Vec3{0.0f, -9.3f, -2.0f};
    return record;
}

CameraSubject standing(const Vec3& feet) {
    return CameraSubject{feet, feet + Vec3{0.0f, 3.0f, 0.0f}};
}

TEST_CASE("the boss camera looks at the boss from behind the party, flatter than the follow "
          "camera, and backs off to keep everyone in view",
          "[game][world][camera]") {
    const BossCameraInfo record = cryptRecord();
    const CameraView view;
    BossCameraSubject lich;
    lich.position = Vec3{0.0f, 0.0f, 0.0f};
    lich.facing = 0.0f; // faces +z, toward the party
    lich.radius = 4.0f;
    lich.awake = false;
    const std::vector<CameraSubject> party{standing(Vec3{0.0f, 0.0f, 30.0f})};
    BossCamera camera;
    camera.reset(lich, party, record, view);
    // Asleep: the camera looks the boss's way along the party's line, at the party.
    REQUIRE(camera.yaw() == Approx(kPi).margin(0.01f));
    REQUIRE(camera.attention() == Vec3{0.0f, 3.0f, 30.0f});
    REQUIRE(camera.pitch() >= record.minPitch);
    REQUIRE(camera.pitch() <= record.maxPitch);
    REQUIRE(camera.distance() >= record.minPlayerDistance);
    REQUIRE(camera.distance() <= record.maxPlayerDistance * BossCamera::kFarthest);
    REQUIRE(camera.camera().position.z > 30.0f); // behind the party
    REQUIRE(camera.margin() >= 0.0f);
    // Awake: it looks at the boss, a little off it, and stands far enough back for the boss
    // and the player both to be on screen.
    lich.awake = true;
    for (int i = 0; i < 600; ++i) {
        camera.update(lich, party, record, view, 1.0f / 60.0f);
    }
    REQUIRE(camera.yaw() == Approx(kPi).margin(0.01f));
    REQUIRE(camera.attention().z == Approx(-2.0f).margin(0.1f));
    REQUIRE(camera.attention().y < 2.3f);
    REQUIRE(camera.attention().y > -9.3f);
    REQUIRE(camera.margin() >= 0.0f);
    REQUIRE(camera.margin() < BossCamera::kLooseMargin + 0.5f);
    REQUIRE(camera.distance() > record.minDistance);
    REQUIRE(camera.camera().position.z > 30.0f);
    // The player walks round to the boss's side: the camera swings round with them, at its
    // rate, to look along their new line.
    const std::vector<CameraSubject> beside{standing(Vec3{30.0f, 0.0f, 0.0f})};
    camera.update(lich, beside, record, view, 1.0f / 60.0f);
    const float turned = std::abs(BossCamera::wrapAngle(camera.yaw() - kPi));
    REQUIRE(turned > 0.0f);
    REQUIRE(turned <= BossCamera::kTurnRate / 60.0f + 0.001f);
    for (int i = 0; i < 600; ++i) {
        camera.update(lich, beside, record, view, 1.0f / 60.0f);
    }
    REQUIRE(camera.yaw() == Approx(-kPi / 2.0f).margin(0.01f)); // looking along -x
    REQUIRE(camera.camera().position.x > 30.0f);
    REQUIRE(camera.margin() >= 0.0f);
    // Two players far apart need a longer view than one.
    const float alone = camera.distance();
    const std::vector<CameraSubject> spread{standing(Vec3{40.0f, 0.0f, 20.0f}),
                                            standing(Vec3{40.0f, 0.0f, -20.0f})};
    for (int i = 0; i < 600; ++i) {
        camera.update(lich, spread, record, view, 1.0f / 60.0f);
    }
    REQUIRE(camera.distance() > alone);
    REQUIRE(camera.margin() >= 0.0f);
    REQUIRE(camera.distance() <= record.maxDistance * BossCamera::kFarthest);
}

TEST_CASE("the boss camera keeps within the boss's facing when the record limits its swing",
          "[game][world][camera]") {
    BossCameraInfo record = cryptRecord();
    record.maxYaw = kPi / 4.0f;
    record.cosMaxYaw = std::cos(kPi / 4.0f);
    const CameraView view;
    BossCameraSubject boss;
    boss.facing = 0.0f;
    boss.awake = true;
    // Straight behind the boss: the line would look along +z from behind it, but the swing
    // is held to a quarter turn off its facing, so the camera looks from its front quarter.
    const std::vector<CameraSubject> behind{standing(Vec3{0.0f, 0.0f, -30.0f})};
    BossCamera camera;
    camera.reset(boss, behind, record, view);
    for (int i = 0; i < 600; ++i) {
        camera.update(boss, behind, record, view, 1.0f / 60.0f);
    }
    const float off = std::abs(BossCamera::wrapAngle(camera.yaw() - (boss.facing + kPi)));
    REQUIRE(off == Approx(kPi / 4.0f).margin(0.02f));
}

TEST_CASE("the dragon camera uses its authored yaw limit and elevated attention anchor",
          "[game][world][camera]") {
    BossCameraInfo record = cryptRecord();
    record.flags = 1;
    record.maxYaw = kPi / 10.0f;             // the mountain's 18 degrees
    record.cosMaxYaw = std::cos(kPi / 4.0f); // stale cache in the level record
    record.minAttention = record.maxAttention = Vec3{0.0f, -5.0f, 0.0f};
    BossCameraSubject boss;
    boss.awake = true;
    boss.attentionOffset = Vec3{0.0f, 18.5f, 0.0f};
    const CameraView view;
    for (const float side : {-1.0f, 1.0f}) {
        const float angle = side * kPi / 6.0f; // 30 degrees: outside 18, inside stale 45
        const std::vector<CameraSubject> party{
            standing(Vec3{30.0f * std::sin(angle), 0.0f, 30.0f * std::cos(angle)})};
        BossCamera camera;
        camera.reset(boss, party, record, view);
        for (int i = 0; i < 300; ++i) {
            camera.update(boss, party, record, view, 1.0f / 60.0f);
        }
        REQUIRE(BossCamera::wrapAngle(camera.yaw() - kPi) ==
                Approx(side * record.maxYaw).margin(0.001f));
        REQUIRE(camera.attention().y == Approx(13.5f));
        REQUIRE(camera.camera().position.x * side > 0.0f);
    }
}

} // namespace
