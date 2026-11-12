#include <cmath>
#include <tuple>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "expansions/n64/camera/LegendsCameraChannel.h"
#include "expansions/n64/camera/LegendsCameraTimes.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Phase = LegendsCameraChannel::Phase;

constexpr f32 kUpdate = test::kCameraUpdate;
constexpr auto kShotTimes = test::kTowerShotTimes;
constexpr auto kReturnTimes = test::kTowerReturnTimes;

/** Advances until the channel rests; the advance that reported it, or 0 within `limit`. */
s32 advancesToRest(LegendsCameraChannel& channel, s32 limit = 200) {
    for (s32 update = 1; update <= limit; ++update) {
        if (channel.advance(kUpdate)) {
            return update;
        }
    }
    return 0;
}

TEST_CASE("N64 camera channel crosses a move from rest in its phases' seconds",
          "[legends][camera][tower-ceremony]") {
    // From rest a move takes acceleration + cruise + braking: 2 s for the shots, 1.8 s for the
    // return, cruising at its length over (acceleration + braking) / 2 + cruise (e007cdf0).
    for (const auto& [times, updates, cruise] :
         {std::tuple{kShotTimes, 60, 5 / 1.5f}, std::tuple{kReturnTimes, 54, 5 / 1.4f}}) {
        CAPTURE(updates);
        LegendsCameraChannel channel;
        channel.set({1, 2, 3}, true, times);
        CHECK(channel.phase() == Phase::Done);
        const Vec3 goal{4, 6, 3};
        channel.set(goal, false, times);
        CHECK(channel.phase() == Phase::Accelerating);
        bool cruised = false;
        for (s32 update = 1; update <= updates; ++update) {
            CAPTURE(update);
            CHECK(channel.advance(kUpdate) == (update == updates));
            CHECK((channel.value() == goal) == (update == updates));
            if (channel.phase() == Phase::Cruising) {
                cruised = true;
                CHECK(channel.speed() == cruise);
            }
        }
        CHECK(cruised);
        CHECK(channel.speed() == 0);
    }
}

TEST_CASE("N64 camera channel keeps a moving speed when it plans again",
          "[legends][camera][tower-ceremony]") {
    LegendsCameraChannel channel;
    channel.set({0, 0, 0}, true, kShotTimes);
    channel.set({10, 0, 0}, false, kShotTimes);
    for (s32 update = 0; update < 30; ++update) {
        channel.advance(kUpdate);
    }
    REQUIRE(channel.phase() == Phase::Cruising);
    const f32 speed = channel.speed();
    CHECK(speed == 10 / 1.5f);
    // e007cdf0 keeps +0x28: the new move starts at the old speed, not from rest.
    channel.set({10, 5, 0}, false, kShotTimes);
    CHECK(channel.speed() == speed);
    CHECK(channel.phase() == Phase::Accelerating);
    const Vec3 from = channel.value();
    channel.advance(kUpdate);
    CHECK(glm::distance(channel.value(), from) > speed * kUpdate * .9f);
    LegendsCameraChannel fresh;
    fresh.set(from, true, kShotTimes);
    fresh.set({10, 5, 0}, false, kShotTimes);
    fresh.advance(kUpdate);
    CHECK(glm::distance(fresh.value(), from) < speed * kUpdate * .1f);
}

TEST_CASE("N64 camera channel drops a move under a thousandth where it stands",
          "[legends][camera][tower-ceremony]") {
    LegendsCameraChannel channel;
    channel.set({0, 0, 0}, true, kShotTimes);
    // Every component under .001: e007d074 keeps the value, and the goal falls back to it.
    channel.set({.0009f, .0009f, .0009f}, false, kShotTimes);
    CHECK(channel.goal() == Vec3{0});
    // Its length is over .001 all the same, so e007cdf0 plans on; e007d774's first update
    // ends the move unmoved.
    CHECK(channel.phase() == Phase::Accelerating);
    CHECK(channel.advance(kUpdate));
    CHECK(channel.value() == Vec3{0});
    CHECK(channel.phase() == Phase::Done);
    // Under .001 in length too, the move is done as it is planned, still where it stood.
    channel.set({.0005f, 0, 0}, false, kShotTimes);
    CHECK(channel.phase() == Phase::Done);
    CHECK(channel.value() == Vec3{0});
    // A component of .001 itself escapes the drop, and the length of .001 snaps to the goal.
    channel.set({.001f, 0, 0}, false, kShotTimes);
    CHECK(channel.phase() == Phase::Done);
    CHECK(channel.value() == Vec3{.001f, 0, 0});
    // Moving, a channel whose every remaining component falls under .001 ends where it is:
    // a short turn's last update is smaller than that.
    LegendsCameraChannel turn{true};
    turn.set({0, 3, 0}, true, kShotTimes);
    turn.set({0, -3, 0}, false, kShotTimes);
    CHECK(advancesToRest(turn) == 60);
    CHECK(turn.value().y != -3);
    CHECK(turn.value().y == Approx(-3).margin(.001f));
    CHECK(turn.goal() == turn.value());
}

TEST_CASE("N64 angular camera channel turns the short way within (-pi, pi]",
          "[legends][camera][tower-ceremony]") {
    // From 3 to -3 the remaining wraps to .28 across pi (e00149d8), not 6 the long way.
    LegendsCameraChannel channel{true};
    channel.set({0, 3, 0}, true, kShotTimes);
    channel.set({0, -3, 0}, false, kShotTimes);
    for (s32 update = 1; update <= 60; ++update) {
        CAPTURE(update);
        channel.advance(kUpdate);
        const f32 yaw = channel.value().y;
        CHECK(yaw > -kPi);
        CHECK(yaw <= kPi);
        CHECK(std::abs(yaw) > 2.9f);
    }
    // A half turn back, -pi, wraps to pi: e00149d8 keeps pi and lifts -pi.
    LegendsCameraChannel half{true};
    half.set({0, 0, 0}, true, kShotTimes);
    half.set({0, -kPi, 0}, false, kShotTimes);
    CHECK(half.goal().y == kPi);
    half.advance(kUpdate);
    CHECK(half.value().y > 0);
}

TEST_CASE("N64 camera channel that is off rests, keeps out of planning and starts where set",
          "[legends][camera][tower-ceremony]") {
    LegendsCameraChannel channel;
    CHECK_FALSE(channel.on());
    CHECK(channel.advance(kUpdate));
    // A move on a channel that is off puts it there at once (the setters' flag-2 branch).
    channel.set({5, 0, 0}, false, kShotTimes);
    CHECK(channel.on());
    CHECK(channel.value() == Vec3{5, 0, 0});
    CHECK(channel.phase() == Phase::Done);
    // Turned off while moving it plans and moves no more, and reports rest.
    channel.set({10, 0, 0}, false, kShotTimes);
    channel.advance(kUpdate);
    const Vec3 stopped = channel.value();
    const f32 speed = channel.speed();
    channel.turnOff();
    channel.plan(kReturnTimes);
    CHECK(channel.phase() == Phase::Accelerating);
    CHECK(channel.advance(kUpdate));
    CHECK(channel.value() == stopped);
    CHECK(channel.speed() == speed);
    // A cut puts the value at the goal, at rest.
    channel.set({1, 1, 1}, true, kShotTimes);
    CHECK(channel.value() == Vec3{1, 1, 1});
    CHECK(channel.speed() == 0);
}
} // namespace
