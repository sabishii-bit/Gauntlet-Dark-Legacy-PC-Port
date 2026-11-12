#include <array>
#include <bit>
#include <optional>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/math/PortableMath.h"
#include "engine/world/WorldCamera.h"

#include "expansions/n64/camera/LegendsCameraRig.h"
#include "expansions/n64/camera/LegendsCameraTimes.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kUpdate = test::kCameraUpdate;
constexpr auto kShotTimes = test::kTowerShotTimes;
constexpr auto kReturnTimes = test::kTowerReturnTimes;

/** e0014d04's rows for the angles (x, y, z), game.bin e0014d04..e0014e94. */
std::array<Vec3, 3> nativeRows(f32 x, f32 y, f32 z) {
    const f32 ax = portable::cos(x);
    const f32 bx = portable::sin(x);
    const f32 ay = portable::cos(y);
    const f32 by = portable::sin(y);
    const f32 az = portable::cos(z);
    const f32 bz = portable::sin(z);
    return {Vec3{ay * az + bx * by * bz, -ax * bz, -by * az + ay * bx * bz},
            Vec3{ay * bz - bx * by * az, ax * az, -by * bz - ay * bx * az},
            Vec3{by * ax, bx, ay * ax}};
}

TEST_CASE("N64 camera rig keeps its angles as e0014d04 reads them in WorldCamera's convention",
          "[legends][camera][tower-ceremony]") {
    // e007d2c0 stores the orbit as (-pitch, yaw, 0); the rotation channel holds the same
    // angles with -roll. Their rows are WorldCamera's right, up and forward.
    for (const f32 pitch : {-1.1f, -.3f, 0.0f, kPi / 6, 1.2f}) {
        for (const f32 yaw : {-3.0f, -1.0f, 0.0f, 1.0f, 2.5f, kPi}) {
            for (const f32 roll : {0.0f, .4f}) {
                CAPTURE(pitch, yaw, roll);
                WorldCamera camera;
                camera.pitch = pitch;
                camera.yaw = yaw;
                camera.roll = roll;
                const auto rows = nativeRows(-pitch, yaw, -roll);
                CHECK(glm::distance(rows[0], camera.right()) < 1e-6f);
                CHECK(glm::distance(rows[1], camera.up()) < 1e-6f);
                CHECK(glm::distance(rows[2], camera.forward()) < 1e-6f);
            }
        }
    }
}

TEST_CASE("N64 camera rig builds its view as it steps, from the centre, orbit and attention",
          "[legends][camera][tower-ceremony]") {
    LegendsCameraRig rig;
    rig.setTimes(kShotTimes);
    WorldCamera gameplay;
    gameplay.position = {20, 30, 40};
    gameplay.pitch = .4f;
    gameplay.yaw = .8f;
    // e007de28 leaves the centre at the eye and the rotation at the angles; nothing shows
    // before e007dd70 builds it.
    rig.settle(gameplay);
    CHECK(rig.view() == WorldCamera{});
    CHECK(rig.step(kUpdate));
    CHECK(rig.view() == gameplay);
    CHECK_FALSE(rig.attention());
    // The orbit puts the eye back from the centre along its pitch and yaw (e007db00); the
    // rotation keeps the turn.
    rig.setOrbit(30, kPi / 6, kPi / 3, true);
    CHECK(rig.view() == gameplay);
    CHECK(rig.step(0));
    WorldCamera along;
    along.pitch = kPi / 6;
    along.yaw = kPi / 3;
    const Vec3 eye = gameplay.position - along.forward() * 30.0f;
    CHECK(rig.view().position == eye);
    CHECK(rig.view().pitch == gameplay.pitch);
    CHECK(rig.view().yaw == gameplay.yaw);
    // A point to look at turns the view to it.
    const Vec3 point{1, 2, 3};
    rig.setAttention(point, true);
    CHECK(rig.step(0));
    CHECK(rig.view().position == eye);
    CHECK(glm::distance(rig.view().forward(), glm::normalize(point - eye)) < 1e-5f);
    CHECK(rig.view().roll == 0);
    CHECK(rig.attention() == point);
    // No point gives the turn back to the rotation, and no rotation either leaves e00a929c's
    // identity.
    rig.setAttention(std::nullopt, true);
    rig.step(0);
    CHECK(rig.view().pitch == gameplay.pitch);
    CHECK(rig.view().yaw == gameplay.yaw);
    rig.setRotation(std::nullopt, true);
    rig.step(0);
    CHECK(rig.view().pitch == 0);
    CHECK(rig.view().yaw == 0);
    CHECK(rig.view().roll == 0);
    CHECK(rig.view().position == eye);
}

TEST_CASE("N64 camera rig plans its moving channels again on new times",
          "[legends][camera][tower-ceremony]") {
    // e007cfd0(0, ...) plans every channel that is on with the new times; a move under way
    // keeps its speed and takes the new phases from there.
    LegendsCameraRig rig;
    rig.setTimes(kShotTimes);
    rig.settle(WorldCamera{});
    rig.setAttention(Vec3{0, 0, 10}, true);
    rig.step(0);
    rig.setAttention(Vec3{0, 0, 20}, false);
    for (s32 update = 0; update < 10; ++update) {
        CHECK_FALSE(rig.step(kUpdate));
    }
    rig.setTimes(kReturnTimes);
    s32 updates = 0;
    while (updates < 200 && !rig.step(kUpdate)) {
        ++updates;
    }
    CHECK(updates + 1 == 54);
}

TEST_CASE("N64 camera rig returns along its orbit to the gameplay eye (e007df20)",
          "[legends][camera][tower-ceremony]") {
    // Sumner's shot at the origin with the party at (3, 0, 12), then case 0x28's return. The
    // values are e007cdf0's, e007d774's and e007db00's arithmetic worked through in float32:
    // the eye arcs back about the party, and the rotation's last remainders fall under .001
    // an update early, leaving its angles a fraction of a milliradian short, as retail's are.
    LegendsCameraRig rig;
    rig.setTimes(kShotTimes);
    WorldCamera gameplay;
    gameplay.position = {20, 30, 40};
    gameplay.pitch = .4f;
    gameplay.yaw = .8f;
    rig.settle(gameplay);
    const Vec3 party{3, 0, 12};
    rig.setCentre(party * .5f, true);
    rig.setOrbit(30, std::bit_cast<f32>(0x3f060a92U), std::bit_cast<f32>(0x3f860a92U), true);
    rig.setAttention(Vec3{0, 3, 0}, true);
    rig.setRotation(std::nullopt, true);
    rig.step(0);
    const auto shot = rig.view();
    rig.setTimes(kReturnTimes);
    rig.setCentre(party, false);
    rig.returnTo(gameplay.position, gameplay.pitch, gameplay.yaw);
    CHECK_FALSE(rig.attention());
    CHECK(rig.view() == shot);
    for (s32 update = 1; update <= 54; ++update) {
        CAPTURE(update);
        CHECK(rig.step(kUpdate) == (update == 54));
        const auto view = rig.view();
        if (update == 1) {
            // The rotation takes over at the look the shot showed.
            CHECK(glm::distance(view.position, Vec3{-21.0323219f, 15.0102158f, -6.94209099f}) <
                  1e-4f);
            CHECK(view.pitch == Approx(.4967421f).margin(1e-5f));
            CHECK(view.yaw == Approx(1.2491014f).margin(1e-5f));
            CHECK(view.pitch == Approx(shot.pitch).margin(1e-3f));
            CHECK(view.yaw == Approx(shot.yaw).margin(1e-3f));
        }
    }
    const auto end = rig.view();
    CHECK(glm::distance(end.position, Vec3{19.9999962f, 29.9999981f, 40.0000076f}) < 1e-4f);
    CHECK(end.pitch == Approx(.400128186f).margin(1e-5f));
    CHECK(end.yaw == Approx(.800594449f).margin(1e-5f));
    CHECK(end.roll == 0);
    CHECK(end.pitch != gameplay.pitch);
}
} // namespace
