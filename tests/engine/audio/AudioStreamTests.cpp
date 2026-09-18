#include <array>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audio/AudioStream.h"

namespace {

using namespace gdl;
using Catch::Matchers::WithinAbs;

constexpr f64 kEpsilon = 1e-5;
constexpr usize kRampFrames = 240; ///< a gain ramp at 48 kHz

TEST_CASE("mono input at the output rate is duplicated to both channels", "[audio][stream]") {
    AudioStream stream(AudioStreamDesc{48000, 1}, 48000);
    const std::array<f32, 4> kInput{0.25f, 0.5f, 0.75f, 1.0f};
    stream.push(kInput);
    CHECK_THAT(stream.queuedSeconds(), WithinAbs(4.0 / 48000.0, kEpsilon));

    std::vector<f32> out(8, 0.0f);
    stream.mixInto(out);
    CHECK_THAT(out[0], WithinAbs(0.25, kEpsilon));
    CHECK_THAT(out[1], WithinAbs(0.25, kEpsilon));
    CHECK_THAT(out[6], WithinAbs(1.0, kEpsilon));
    CHECK_THAT(out[7], WithinAbs(1.0, kEpsilon));
}

TEST_CASE("lower input rates are upsampled along a curve through the frames",
          "[audio][stream]") {
    // A ramp comes through as itself, its frames hit and the halfway points between them.
    AudioStream ramp(AudioStreamDesc{24000, 1}, 48000);
    const std::array<f32, 5> kRamp{0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    ramp.push(kRamp);
    std::vector<f32> out(20, 0.0f);
    ramp.mixInto(out);
    CHECK_THAT(out[0], WithinAbs(0.0, kEpsilon));
    CHECK_THAT(out[4], WithinAbs(0.25, kEpsilon));
    CHECK_THAT(out[6], WithinAbs(0.375, kEpsilon));
    CHECK_THAT(out[8], WithinAbs(0.5, kEpsilon));
    CHECK_THAT(out[10], WithinAbs(0.625, kEpsilon));
    CHECK_THAT(out[12], WithinAbs(0.75, kEpsilon));
    // A lone peak is rounded off rather than made a straight-sided spike.
    AudioStream spike(AudioStreamDesc{24000, 1}, 48000);
    const std::array<f32, 5> kSpike{0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    spike.push(kSpike);
    out.assign(20, 0.0f);
    spike.mixInto(out);
    CHECK_THAT(out[8], WithinAbs(1.0, kEpsilon));
    CHECK(out[10] > 0.5f);
    CHECK(out[10] < 0.7f);
    CHECK_THAT(out[10], WithinAbs(out[6], kEpsilon)); // symmetric about the peak
    CHECK_THAT(out[16], WithinAbs(0.0, kEpsilon));
}

TEST_CASE("stereo input keeps its channels and mixing adds to existing samples",
          "[audio][stream]") {
    AudioStream stream(AudioStreamDesc{48000, 2}, 48000);
    const std::array<f32, 4> kInput{0.5f, -0.5f, 0.5f, -0.5f};
    stream.push(kInput);
    std::vector<f32> out{0.25f, 0.25f, 0.25f, 0.25f};
    stream.mixInto(out);
    CHECK_THAT(out[0], WithinAbs(0.75, kEpsilon));
    CHECK_THAT(out[1], WithinAbs(-0.25, kEpsilon));
    CHECK_THAT(out[3], WithinAbs(-0.25, kEpsilon));
}

TEST_CASE("volume scales the output and underruns leave silence", "[audio][stream]") {
    AudioStream stream(AudioStreamDesc{48000, 1}, 48000);
    stream.setVolume(0.5f);
    REQUIRE(stream.volume() == 0.5f);
    const std::array<f32, 1> kInput{1.0f};
    stream.push(kInput);
    std::vector<f32> out(6, 0.0f);
    stream.mixInto(out);
    CHECK_THAT(out[0], WithinAbs(0.5, kEpsilon));
    CHECK_THAT(out[2], WithinAbs(0.0, kEpsilon));
    CHECK_THAT(out[4], WithinAbs(0.0, kEpsilon));
}

TEST_CASE("pan moves a sound between the speakers at constant power, sliding there",
          "[audio][stream]") {
    AudioStream stream(AudioStreamDesc{48000, 1}, 48000);
    REQUIRE(stream.pan() == 0.0f);
    const std::vector<f32> kInput(4000, 1.0f);
    stream.push(kInput);
    std::vector<f32> out(2, 0.0f);
    stream.mixInto(out);
    CHECK_THAT(out[0], WithinAbs(1.0, kEpsilon)); // centred: both speakers as they were
    CHECK_THAT(out[1], WithinAbs(1.0, kEpsilon));
    stream.setPan(-1.0f);
    out.assign(usize{4} * kRampFrames, 0.0f);
    stream.mixInto(out);
    // The first frame has moved a step only; by the end of the ramp the sound is hard left.
    CHECK(out[0] > 1.0f);
    CHECK(out[0] < 1.01f);
    CHECK(out[1] < 1.0f);
    CHECK(out[1] > 0.99f);
    CHECK_THAT(out[2 * kRampFrames], WithinAbs(std::numbers::sqrt2, 1e-4));
    CHECK_THAT(out[2 * kRampFrames + 1], WithinAbs(0.0, 1e-4));
    CHECK_THAT(out[out.size() - 2], WithinAbs(std::numbers::sqrt2, 1e-4));
    stream.setPan(3.0f); // clamped to hard right
    REQUIRE(stream.pan() == 1.0f);
    out.assign(usize{4} * kRampFrames, 0.0f);
    stream.mixInto(out);
    CHECK_THAT(out[out.size() - 2], WithinAbs(0.0, 1e-4));
    CHECK_THAT(out[out.size() - 1], WithinAbs(std::numbers::sqrt2, 1e-4));
}

TEST_CASE("a volume change slides over the ramp rather than stepping", "[audio][stream]") {
    AudioStream stream(AudioStreamDesc{48000, 1}, 48000);
    const std::vector<f32> kInput(4000, 1.0f);
    stream.push(kInput);
    std::vector<f32> out(2, 0.0f);
    stream.mixInto(out);
    CHECK_THAT(out[0], WithinAbs(1.0, kEpsilon));
    stream.setVolume(0.0f);
    out.assign(2 * kRampFrames, 0.0f);
    stream.mixInto(out);
    for (usize frame = 1; frame < kRampFrames; ++frame) {
        REQUIRE(out[frame * 2] < out[(frame - 1) * 2]);
    }
    CHECK_THAT(out[2 * (kRampFrames - 1)], WithinAbs(0.0, kEpsilon));
    CHECK_THAT(out[2 * (kRampFrames / 2)], WithinAbs(0.5, 0.01));
}

TEST_CASE("a finished stream drains once its queue is consumed", "[audio][stream]") {
    AudioStream stream(AudioStreamDesc{48000, 1}, 48000);
    const std::array<f32, 2> kInput{0.1f, 0.2f};
    stream.push(kInput);
    REQUIRE_FALSE(stream.finished());
    stream.finish();
    REQUIRE(stream.finished());
    REQUIRE_FALSE(stream.drained());
    std::vector<f32> out(4, 0.0f);
    stream.mixInto(out);
    REQUIRE(stream.drained());
    REQUIRE(stream.queuedSeconds() == 0.0);
}

TEST_CASE("stopping fades the sound away over the ramp and drops the rest", "[audio][stream]") {
    AudioStream stream(AudioStreamDesc{48000, 1}, 48000);
    const std::vector<f32> kInput(48000, 1.0f);
    stream.push(kInput);
    std::vector<f32> out(2, 0.0f);
    stream.mixInto(out);
    CHECK_THAT(out[0], WithinAbs(1.0, kEpsilon));
    stream.stop();
    REQUIRE(stream.finished());
    REQUIRE_FALSE(stream.drained()); // the fade is still to play
    REQUIRE(stream.queuedSeconds() <= AudioStream::kGainRamp + 1e-4);
    out.assign(usize{4} * kRampFrames, 0.0f);
    stream.mixInto(out);
    for (usize frame = 1; frame < kRampFrames; ++frame) {
        REQUIRE(out[frame * 2] < out[(frame - 1) * 2]);
    }
    CHECK_THAT(out[2 * (kRampFrames - 1)], WithinAbs(0.0, kEpsilon));
    CHECK_THAT(out[2 * (kRampFrames + 60)], WithinAbs(0.0, kEpsilon));
    REQUIRE(stream.drained());
    REQUIRE(stream.queuedSeconds() == 0.0);
    // Stopped before it was ever heard, a stream plays nothing at all.
    AudioStream unheard(AudioStreamDesc{48000, 1}, 48000);
    unheard.push(kInput);
    unheard.stop();
    out.assign(8, 0.0f);
    unheard.mixInto(out);
    for (const f32 sample : out) {
        CHECK_THAT(sample, WithinAbs(0.0, kEpsilon));
    }
}

} // namespace
