#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/world/StartCamera.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

WorldCamera markerAbove() {
    WorldCamera marker;
    marker.position = Vec3{0.0f, 20.0f, 0.0f};
    marker.pitch = 0.5f;
    marker.yaw = kPi; // looking down the way the party stands, along -z
    marker.roll = 0.02f;
    return marker;
}

void requireNear(const Vec3& actual, const Vec3& expected, f32 margin = 1e-3f) {
    REQUIRE(actual.x == Approx(expected.x).margin(margin));
    REQUIRE(actual.y == Approx(expected.y).margin(margin));
    REQUIRE(actual.z == Approx(expected.z).margin(margin));
}

TEST_CASE("the start camera holds at the marker, then rides at its pace to the follow camera",
          "[game][world][camera]") {
    const WorldCamera marker = markerAbove();
    const Vec3 party{0.0f, 0.0f, -30.0f};
    const Vec3 followPosition{0.0f, 14.0f, -10.0f};
    const Vec3 followAttention{0.0f, 2.5f, -30.0f};
    StartCamera camera;
    REQUIRE_FALSE(camera.active());
    REQUIRE_FALSE(camera.update(1, false, followPosition, followAttention));

    camera.start(marker, party);
    REQUIRE(camera.active());
    REQUIRE(camera.phase() == StartCamera::Phase::Hold);
    REQUIRE(camera.ticksLeft() == StartCamera::kHoldTicks);
    // It stands at the marker, level, looking the marker's way as far off as the party.
    requireNear(camera.camera().position, marker.position);
    REQUIRE(camera.camera().pitch == Approx(0.5f));
    REQUIRE(camera.camera().yaw == Approx(kPi));
    REQUIRE(camera.camera().roll == 0.0f);
    WorldCamera level = marker;
    level.roll = 0.0f;
    requireNear(camera.attention(),
                marker.position + level.forward() * glm::distance(marker.position, party));

    // The hold counts down; a button means nothing until most of it has passed.
    REQUIRE(camera.update(2, true, followPosition, followAttention));
    REQUIRE(camera.ticksLeft() == StartCamera::kHoldTicks - 2);
    REQUIRE(camera.phase() == StartCamera::Phase::Hold);
    requireNear(camera.camera().position, marker.position);
    REQUIRE(camera.update(camera.ticksLeft() - StartCamera::kSkipBelow, true, followPosition,
                          followAttention));
    REQUIRE(camera.ticksLeft() == StartCamera::kSkipBelow);
    REQUIRE(camera.phase() == StartCamera::Phase::Hold);
    REQUIRE(camera.update(1, true, followPosition, followAttention));
    REQUIRE(camera.ticksLeft() == 0);
    REQUIRE(camera.phase() == StartCamera::Phase::Ride);

    // The ride moves it straight at the follow camera a pace a tick, looking at a point that
    // slides to the follow camera's the same way.
    const Vec3 before = camera.camera().position;
    const Vec3 lookedAt = camera.attention();
    REQUIRE(camera.update(1, false, followPosition, followAttention));
    REQUIRE(glm::distance(camera.camera().position, before) ==
            Approx(StartCamera::kUnitsPerTick).margin(1e-3f));
    requireNear(camera.camera().position,
                before + glm::normalize(followPosition - before) * StartCamera::kUnitsPerTick);
    requireNear(camera.attention(),
                lookedAt + glm::normalize(followAttention - lookedAt) * StartCamera::kUnitsPerTick);
    requireNear(camera.camera().forward(),
                glm::normalize(camera.attention() - camera.camera().position), 1e-4f);
    REQUIRE(camera.camera().pitch > 0.0f); // still looking down

    // Within a fraction of a unit of both it hands over, the twelve units of the gap covered
    // at its pace.
    int ticks = 1;
    while (camera.active() && ticks < 400) {
        camera.update(1, false, followPosition, followAttention);
        ++ticks;
    }
    REQUIRE_FALSE(camera.active());
    REQUIRE(ticks <= static_cast<int>(12.0f / StartCamera::kUnitsPerTick) + 2);
    REQUIRE(glm::distance(camera.camera().position, followPosition) < StartCamera::kArrival);
    REQUIRE(glm::distance(camera.attention(), followAttention) < StartCamera::kArrival);
    REQUIRE_FALSE(camera.update(1, false, followPosition, followAttention));
}

TEST_CASE("the start camera's hold runs its course without a button and it rides faster from far off",
          "[game][world][camera]") {
    const WorldCamera marker = markerAbove();
    const Vec3 followPosition{0.0f, 14.0f, -300.0f};
    const Vec3 followAttention{0.0f, 2.5f, -320.0f};
    StartCamera camera;
    camera.start(marker, Vec3{0.0f, 0.0f, -30.0f});
    REQUIRE(camera.update(StartCamera::kHoldTicks - 1, false, followPosition, followAttention));
    REQUIRE(camera.ticksLeft() == 1);
    REQUIRE(camera.phase() == StartCamera::Phase::Hold);
    REQUIRE(camera.update(1, false, followPosition, followAttention));
    REQUIRE(camera.phase() == StartCamera::Phase::Ride);

    // Farther than the reach, a tick covers the gap's share of it: the position's over two
    // hundred units, the look-at point's over twenty.
    const Vec3 before = camera.camera().position;
    const Vec3 lookedAt = camera.attention();
    const f32 positionGap = glm::distance(before, followPosition);
    const f32 attentionGap = glm::distance(lookedAt, followAttention);
    REQUIRE(positionGap > StartCamera::kPositionReach);
    REQUIRE(attentionGap > StartCamera::kAttentionReach);
    REQUIRE(camera.update(1, false, followPosition, followAttention));
    REQUIRE(glm::distance(camera.camera().position, before) ==
            Approx(positionGap / StartCamera::kPositionReach * StartCamera::kUnitsPerTick)
                .margin(1e-3f));
    REQUIRE(glm::distance(camera.attention(), lookedAt) ==
            Approx(attentionGap / StartCamera::kAttentionReach * StartCamera::kUnitsPerTick)
                .margin(1e-3f));
    // A late frame's ticks cover that many paces, never past the target.
    camera.stop();
    camera.start(marker, Vec3{0.0f, 0.0f, -30.0f});
    camera.update(StartCamera::kHoldTicks, false, followPosition, followAttention);
    const Vec3 near{0.0f, 20.0f, -0.5f};
    camera.update(4, false, near, camera.attention());
    requireNear(camera.camera().position, near);
}

} // namespace
