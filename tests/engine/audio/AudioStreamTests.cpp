#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audio/AudioStream.h"

namespace {

using namespace gdl;
using Catch::Matchers::WithinAbs;

constexpr f64 kEpsilon = 1e-5;

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

TEST_CASE("lower input rates are upsampled linearly", "[audio][stream]") {
    AudioStream stream(AudioStreamDesc{24000, 1}, 48000);
    const std::array<f32, 3> kInput{0.0f, 1.0f, 0.0f};
    stream.push(kInput);

    std::vector<f32> out(12, 0.0f);
    stream.mixInto(out);
    CHECK_THAT(out[0], WithinAbs(0.0, kEpsilon));
    CHECK_THAT(out[2], WithinAbs(0.5, kEpsilon));
    CHECK_THAT(out[4], WithinAbs(1.0, kEpsilon));
    CHECK_THAT(out[6], WithinAbs(0.5, kEpsilon));
    CHECK_THAT(out[8], WithinAbs(0.0, kEpsilon));
    CHECK_THAT(out[10], WithinAbs(0.0, kEpsilon));
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

} // namespace
