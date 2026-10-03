#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/world/BossCamera.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

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

TEST_CASE("a sleeping boss does not pull the approach camera away from the stairs",
          "[game][world][camera][chimera]") {
    const auto record = cryptRecord();
    const std::vector<CameraSubject> party{standing({0, -7, 38})};
    BossCameraSubject boss;
    boss.position = Vec3{0, 24, -48};
    BossCamera camera;
    camera.reset(boss, party, record, {});
    const f32 distance = camera.distance();
    boss.position.y += 200;
    camera.reset(boss, party, record, {});
    CHECK(camera.distance() == Approx(distance));
    CHECK(distance == Approx(record.minPlayerDistance));
    CHECK(camera.attention() == party[0].follow);
}

TEST_CASE("boss approach follows horizontal camera markers until the boss wakes",
          "[game][world][camera][chimera]") {
    auto record = cryptRecord();
    record.maxYaw = kPi;
    BossCameraSubject boss;
    boss.position = Vec3{0, 24, -48};
    std::vector<CameraSubject> party{standing({0, -7, 38})};
    std::vector<WorldLocator> markers(2);
    markers[0].position = Vec3{0, 200, 38}; // altitude must not change the selection
    markers[0].rotation = Vec3{0.645772f, 2.8f, 0};
    markers[1].position = Vec3{0, 0, 10};
    markers[1].rotation = Vec3{0.523599f, kPi, 0};
    BossCamera camera;
    camera.reset(boss, party, record, {}, markers);
    CHECK(camera.yaw() == Approx(markers[0].rotation.y));
    CHECK(camera.pitch() == Approx(markers[0].rotation.x));
    party[0] = standing({0, 0, 23}); // slightly nearer second, but within hysteresis
    camera.update(boss, party, record, {}, 1.0f / 60, markers);
    CHECK(camera.yaw() == Approx(markers[0].rotation.y));
    party[0] = standing({0, 0, 10});
    for (s32 frame = 0; frame < 600; ++frame) {
        camera.update(boss, party, record, {}, 1.0f / 60, markers);
    }
    CHECK(std::abs(BossCamera::wrapAngle(camera.yaw() - kPi)) < 0.001f);
    CHECK(camera.pitch() == Approx(markers[1].rotation.x).margin(0.001f));
    boss.awake = true;
    const f32 before = camera.pitch();
    camera.update(boss, party, record, {}, 1.0f / 60, markers);
    CHECK(std::abs(camera.pitch() - before) < 0.001f);
    for (s32 frame = 0; frame < 600; ++frame) {
        camera.update(boss, party, record, {}, 1.0f / 60, markers);
    }
    CHECK(camera.pitch() < markers[1].rotation.x - 0.05f);
}

TEST_CASE("boss framing keeps the player's collision envelope inside the view",
          "[game][world][camera][chimera]") {
    const auto record = cryptRecord();
    const CameraView view;
    BossCameraSubject boss;
    boss.awake = true;
    std::vector<CameraSubject> party{standing({0, 0, 70})};
    party[0].viewRadius = 3;
    BossCamera body;
    body.reset(boss, party, record, view);
    for (s32 i = 0; i < 600; ++i) {
        body.update(boss, party, record, view, 1.0f / 60);
    }
    CHECK(body.margin() >= 0);
    const Vec3 relative = party[0].follow - body.camera().position;
    const f32 depth = glm::dot(relative, body.camera().forward());
    const f32 up = glm::dot(relative, body.camera().up());
    const f32 tanY = std::tan(view.horizontalFov * 0.5f) / view.aspect;
    const f32 bottomGap = (depth * tanY + up) / std::sqrt(1 + tanY * tanY);
    CHECK(bottomGap >= party[0].viewRadius);
}

TEST_CASE("native Chimera camera frames the player body at the front of the arena",
          "[game][world][camera][chimera][assets]") {
    WorldData data;
    REQUIRE(data.load(test::assetOrSkip("WDATA/CASTLE.WAD")));
    const auto level =
        std::ranges::find_if(data.levels(), [](const auto& value) { return value.name == "A5"; });
    REQUIRE(level != data.levels().end());
    REQUIRE(level->bossCamera.has_value());
    const auto& record = *level->bossCamera;
    CHECK(record.minDistance == 56);
    CHECK(record.maxDistance == 135);
    CHECK(record.minPitch == Approx(25.0f * kPi / 180.0f));
    BossCameraSubject boss;
    boss.awake = true;
    boss.height = 20;
    boss.attentionOffset = Vec3{0, 20, 0};
    std::vector<CameraSubject> party{standing({0, 0, 70})};
    BossCamera pointCamera;
    pointCamera.reset(boss, party, record, {});
    for (s32 i = 0; i < 600; ++i) {
        pointCamera.update(boss, party, record, {}, 1.0f / 60);
    }
    party[0].viewRadius = 3;
    BossCamera bodyCamera;
    bodyCamera.reset(boss, party, record, {});
    for (s32 i = 0; i < 600; ++i) {
        bodyCamera.update(boss, party, record, {}, 1.0f / 60);
    }
    CHECK(bodyCamera.distance() > pointCamera.distance());
    CHECK(bodyCamera.margin() >= 0);
    CHECK(bodyCamera.pitch() == Approx(record.minPitch).margin(0.001f));
}

TEST_CASE("victory camera uses wizard and shard offsets instead of combat attention",
          "[game][world][camera]") {
    auto record = cryptRecord();
    record.flags |= 0x10;
    record.wizardAttention = Vec3{1, 2, 3};
    record.keyAttention = Vec3{-1, 4, -3};
    const std::vector<CameraSubject> party{standing({0, 0, 30})};
    BossCameraSubject subject;
    subject.position = Vec3{0, 3, 5};
    subject.attentionOffset = Vec3{0, 10, 0};
    subject.focus = BossCameraSubject::Focus::Wizard;
    subject.awake = true;
    BossCamera camera;
    camera.reset(subject, party, record, {});
    REQUIRE(camera.attention() == Vec3{1, 15, 8});
    subject.focus = BossCameraSubject::Focus::Shard;
    subject.attentionOffset = Vec3{0};
    camera.reset(subject, party, record, {});
    REQUIRE(camera.attention() == Vec3{-1, 7, 2});
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
    for (s32 i = 0; i < 600; ++i) {
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
    const f32 turned = std::abs(BossCamera::wrapAngle(camera.yaw() - kPi));
    REQUIRE(turned > 0.0f);
    REQUIRE(turned <= BossCamera::kTurnRate / 60.0f + 0.001f);
    for (s32 i = 0; i < 600; ++i) {
        camera.update(lich, beside, record, view, 1.0f / 60.0f);
    }
    REQUIRE(camera.yaw() == Approx(-kPi / 2.0f).margin(0.01f)); // looking along -x
    REQUIRE(camera.camera().position.x > 30.0f);
    REQUIRE(camera.margin() >= 0.0f);
    // Two players far apart need a longer view than one.
    const f32 alone = camera.distance();
    const std::vector<CameraSubject> spread{standing(Vec3{40.0f, 0.0f, 20.0f}),
                                            standing(Vec3{40.0f, 0.0f, -20.0f})};
    for (s32 i = 0; i < 600; ++i) {
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
    for (s32 i = 0; i < 600; ++i) {
        camera.update(boss, behind, record, view, 1.0f / 60.0f);
    }
    const f32 off = std::abs(BossCamera::wrapAngle(camera.yaw() - (boss.facing + kPi)));
    REQUIRE(off == Approx(kPi / 4.0f).margin(0.02f));
}

TEST_CASE("boss pitch accelerates from rest when waking instead of snapping to the fight angle",
          "[game][world][camera][boss-pitch]") {
    const f32 seconds = GENERATE(1.0f / 30.0f, 1.0f / 60.0f);
    const bool rising = GENERATE(false, true);
    CAPTURE(seconds, rising);
    auto record = cryptRecord();
    record.minPlayerDistance = record.maxPlayerDistance = 60.0f;
    if (rising) {
        std::swap(record.minPitch, record.maxPitch);
    }
    BossCameraSubject boss;
    const std::vector<CameraSubject> party{standing({0, 0, 30})};
    BossCamera camera;
    camera.reset(boss, party, record, {});
    const f32 asleepPitch = camera.pitch();
    boss.awake = true;
    camera.update(boss, party, record, {}, 0.0f);
    REQUIRE(camera.pitch() == asleepPitch);
    camera.update(boss, party, record, {}, seconds);
    const f32 firstStep = camera.pitch() - asleepPitch;
    CHECK(std::abs(firstStep) ==
          Approx(BossCamera::kPitchAcceleration * seconds * seconds).margin(1.0e-7f));
    CHECK((rising ? firstStep : -firstStep) > 0.0f);
    for (s32 frame = 0; frame < 600; ++frame) {
        const f32 before = camera.pitch();
        camera.update(boss, party, record, {}, seconds);
        REQUIRE(std::isfinite(camera.pitch()));
        REQUIRE(std::abs(BossCamera::wrapAngle(camera.pitch() - before)) <=
                BossCamera::kPitchSpeed * seconds + 1.0e-6f);
    }
    CHECK(std::abs(camera.pitch() - asleepPitch) > 0.02f);
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
    for (const f32 side : {-1.0f, 1.0f}) {
        const f32 angle = side * kPi / 6.0f; // 30 degrees: outside 18, inside stale 45
        const std::vector<CameraSubject> party{
            standing(Vec3{30.0f * std::sin(angle), 0.0f, 30.0f * std::cos(angle)})};
        BossCamera camera;
        camera.reset(boss, party, record, view);
        for (s32 i = 0; i < 300; ++i) {
            camera.update(boss, party, record, view, 1.0f / 60.0f);
        }
        REQUIRE(BossCamera::wrapAngle(camera.yaw() - kPi) ==
                Approx(side * record.maxYaw).margin(0.001f));
        REQUIRE(camera.attention().y == Approx(13.5f));
        REQUIRE(camera.camera().position.x * side > 0.0f);
    }
}

TEST_CASE("the genie camera selects the base anchor without vertical drift",
          "[game][world][camera][genie]") {
    BossCameraInfo record = cryptRecord();
    record.flags = 2;
    record.minDistance = 50;
    record.maxDistance = 65;
    record.minPitch = 0.2617994f;
    record.maxPitch = 0.19198622f;
    record.minAttention = Vec3{0, 13, 0};
    record.maxAttention = Vec3{0, 9, 0};
    BossCameraSubject boss;
    boss.awake = true;
    boss.position = Vec3{10, 0, 20};
    boss.baseAttention = Vec3{10, 7, 20};
    boss.attentionOffset = Vec3{0, 17, 0};
    const std::vector<CameraSubject> party{standing(Vec3{10, 0, 50})};
    BossCamera camera;
    camera.reset(boss, party, record, CameraView{});
    REQUIRE(camera.attention().y >= 16);
    REQUIRE(camera.attention().y <= 20);
    // Hold the authored offset fixed to isolate anchor selection from distance easing.
    record.maxAttention = record.minAttention;
    camera.reset(boss, party, record, CameraView{});
    REQUIRE(camera.attention().y == Approx(20));
    record.flags |= 1;
    camera.reset(boss, party, record, CameraView{});
    REQUIRE(camera.attention().y == Approx(30));
    record.flags = 0x10;
    record.minAttention = record.maxAttention = Vec3{0};
    camera.reset(boss, party, record, CameraView{});
    REQUIRE(camera.attention() == party[0].follow);
}

} // namespace
