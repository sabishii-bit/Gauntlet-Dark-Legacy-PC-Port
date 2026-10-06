
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"

namespace {

using namespace gdl;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

TreeSequenceInfo sequence(s32 frames, s32 rate) {
    TreeSequenceInfo info;
    info.name = "TEST";
    info.frames = frames;
    info.frameRate = rate;
    return info;
}

TEST_CASE("a sequence steps a frame per tick, wraps when it repeats and holds when it does not",
          "[world][animation]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    AnimationPlayer player;
    REQUIRE_FALSE(player.playing());
    player.start(cycle, 3);
    REQUIRE(player.playing());
    REQUIRE(player.sequence() == 3);
    REQUIRE(player.frame() == 0.0f);
    REQUIRE(player.secondsPerFrame() == Approx(kStep));
    for (s32 i = 1; i <= 11; ++i) {
        REQUIRE_FALSE(player.advance(kStep, true));
        REQUIRE(player.frame() == Approx(static_cast<f32>(i)));
        REQUIRE_FALSE(player.finished());
    }
    // The step past the last frame wraps to the first and reports the loop.
    REQUIRE(player.advance(kStep, true));
    REQUIRE(player.finished());
    REQUIRE(player.frame() == 0.0f);
    REQUIRE_FALSE(player.advance(kStep, true));
    REQUIRE_FALSE(player.finished());
    REQUIRE(player.frame() == 1.0f);

    // Not repeating, the same step lands on the last frame and stays there.
    player.start(cycle, 3);
    for (s32 i = 0; i < 11; ++i) {
        player.advance(kStep, false);
    }
    REQUIRE(player.advance(kStep, false));
    REQUIRE(player.finished());
    REQUIRE(player.frame() == 11.0f);
    REQUIRE_FALSE(player.advance(kStep, false));
    REQUIRE(player.finished());
    REQUIRE(player.frame() == 11.0f);
}

TEST_CASE("a sequence's rate is 900 over its frames per second and frames snap to whole numbers",
          "[world][animation]") {
    const TreeSequenceInfo slow = sequence(150, 45); // twenty frames a second
    AnimationPlayer player;
    player.start(slow, 0);
    REQUIRE(player.secondsPerFrame() == Approx(0.05f));
    player.advance(kStep, false);
    REQUIRE(player.frame() == 1.0f); // two thirds of a frame rounds up
    player.advance(kStep, false);
    REQUIRE(player.frame() == 1.0f);
    player.advance(kStep, false);
    REQUIRE(player.frame() == 2.0f);

    // Smooth playback keeps the fraction when it is far from a whole frame.
    player.setSmooth(true);
    player.start(slow, 0);
    player.advance(kStep, false);
    REQUIRE(player.frame() == Approx(2.0f / 3.0f));
    player.advance(kStep, false);
    player.advance(kStep, false);
    REQUIRE(player.frame() == 2.0f);

    // A rate of 0 plays at thirty a second; a doubled speed halves the frame time.
    // Playback borrows the sequence, so the record must outlive every advance.
    const TreeSequenceInfo fallback = sequence(10, 0);
    const TreeSequenceInfo normal = sequence(10, 30);
    player.setSmooth(false);
    player.start(fallback, 0);
    REQUIRE(player.secondsPerFrame() == Approx(kStep));
    player.setSpeed(2.0f);
    player.start(normal, 0);
    REQUIRE(player.secondsPerFrame() == Approx(kStep / 2.0f));
    player.advance(kStep, false);
    REQUIRE(player.frame() == 2.0f);
}

TEST_CASE("a transition holds the first frame while it blends in", "[world][animation]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    AnimationPlayer player;
    player.start(cycle, 0, 2.0f * kStep);
    REQUIRE(player.transitioning());
    REQUIRE(player.transition() == 0.0f);
    REQUIRE_FALSE(player.advance(kStep, true));
    REQUIRE(player.transition() == Approx(0.5f));
    REQUIRE(player.frame() == 0.0f);
    player.advance(kStep, true);
    REQUIRE_FALSE(player.transitioning());
    REQUIRE(player.transition() == 1.0f);
    REQUIRE(player.frame() == 0.0f);
    player.advance(kStep, true);
    REQUIRE(player.frame() == 1.0f);

    // Without a transition the blend is already complete.
    player.start(cycle, 0);
    REQUIRE_FALSE(player.transitioning());
    REQUIRE(player.transition() == 1.0f);
}

TEST_CASE("an empty sequence is finished at once and a stopped player plays nothing",
          "[world][animation]") {
    const TreeSequenceInfo empty = sequence(0, 30);
    AnimationPlayer player;
    player.start(empty, 0);
    REQUIRE_FALSE(player.advance(kStep, true));
    REQUIRE(player.finished());
    player.stop();
    REQUIRE_FALSE(player.playing());
    REQUIRE(player.frameCount() == 0);
    REQUIRE_FALSE(player.advance(kStep, true));
}

TEST_CASE("initial transition credit affects only the blend and never advances playback",
          "[world][animation][transition-credit]") {
    const auto cycle = sequence(12, 30);
    for (const f32 speed : {0.5f, 1.0f, 2.0f}) {
        CAPTURE(speed);
        AnimationPlayer player;
        player.setSpeed(speed);
        player.setSmooth(true);
        player.start(cycle, 0, 2.0f * kStep, 5.0f, kStep);
        const auto generation = player.generation();
        CHECK(player.frame() == 5.0f);
        CHECK(player.presentationFrame() == Approx(5.0f));
        CHECK(player.transition() == Approx(0.5f));
        CHECK_FALSE(player.finished());
        CHECK_FALSE(player.advance(kStep, false));
        CHECK_FALSE(player.transitioning());
        CHECK(player.frame() == 5.0f);
        CHECK(player.generation() == generation);
        CHECK_FALSE(player.advance(kStep, false));
        CHECK(player.frame() == Approx(5.0f + speed));
    }
    for (const f32 duration : {0.0f, kStep / 2.0f, kStep}) {
        CAPTURE(duration);
        AnimationPlayer player;
        player.start(cycle, 0, duration, 11.0f, kStep);
        CHECK_FALSE(player.transitioning());
        CHECK_FALSE(player.finished());
        CHECK(player.frame() == 11.0f);
        CHECK(player.presentationFrame() == Approx(11.0f));
    }
    AnimationPlayer player;
    player.start(cycle, 0, kStep, 0, -kStep);
    CHECK(player.transition() == 0.0f);
    player.start(cycle, 0, kStep);
    CHECK(player.transition() == 0.0f);
}

TEST_CASE("looping animation owns a full interval for its last frame and keeps overshoot",
          "[world][animation][cadence]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    AnimationPlayer player;
    player.start(cycle, 0);
    REQUIRE_FALSE(player.advance(11.5f / 30.0f, true));
    REQUIRE(player.frame() == 11.0f);
    REQUIRE(player.advance(2.5f / 30.0f, true));
    REQUIRE(player.frame() == 2.0f);
    REQUIRE(player.advance(26.0f / 30.0f, true));
    REQUIRE(player.frame() == 4.0f);
}

TEST_CASE("loop and transition clocks agree across update cadences",
          "[world][animation][cadence]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    for (const s32 rate : {30, 60, 144, 240}) {
        CAPTURE(rate);
        AnimationPlayer player;
        // Deliberately not a multiple of any update interval.
        player.start(cycle, 0, 0.073f);
        s32 wraps = 0;
        for (s32 frame = 0; frame < rate * 2; ++frame) {
            wraps += player.advance(1.0f / static_cast<f32>(rate), true) ? 1 : 0;
        }
        REQUIRE(wraps == 4);
        REQUIRE(player.transition() == 1.0f);
        REQUIRE(player.frame() == 10.0f);
    }
}

TEST_CASE("fractional presentation samples never advance rounded gameplay frames",
          "[world][animation][presentation]") {
    const TreeSequenceInfo cycle = sequence(12, 30);
    AnimationPlayer player;
    player.start(cycle, 0);
    const u64 generation = player.generation();
    player.advance(1.0f / 60.0f, true);
    REQUIRE(player.frame() == 1.0f);
    for (s32 draw = 0; draw < 8; ++draw) {
        REQUIRE(player.presentationFrame() == Approx(0.5f));
        REQUIRE(player.frame() == 1.0f);
        REQUIRE(player.generation() == generation);
        REQUIRE_FALSE(player.finished());
    }
    player.advance(11.5f / 30.0f, true);
    REQUIRE(player.generation() != generation);
    REQUIRE(player.presentationFrame() == Approx(0.0f).margin(0.00001f));
    const u64 wrapped = player.generation();
    player.start(cycle, 0, 0.1f);
    REQUIRE(player.generation() != wrapped);
    player.advance(0.05f, false);
    REQUIRE(player.presentationFrame() == 0.0f);
    player.advance(1.0f, false);
    REQUIRE(player.presentationFrame() == 11.0f);
    player.stop();
    REQUIRE(player.presentationFrame() == 0.0f);
}

} // namespace
