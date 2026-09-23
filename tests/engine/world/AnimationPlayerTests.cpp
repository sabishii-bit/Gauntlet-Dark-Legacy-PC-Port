#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/world/AnimationPlayer.h"

namespace {

using namespace gdl;
using Catch::Approx;

constexpr float kStep = 1.0f / 30.0f;

TreeSequenceInfo sequence(std::int32_t frames, std::int32_t rate) {
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
    for (int i = 1; i <= 11; ++i) {
        REQUIRE_FALSE(player.advance(kStep, true));
        REQUIRE(player.frame() == Approx(static_cast<float>(i)));
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
    for (int i = 0; i < 11; ++i) {
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

} // namespace
