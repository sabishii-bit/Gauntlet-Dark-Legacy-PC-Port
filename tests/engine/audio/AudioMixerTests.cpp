#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audio/AudioMixer.h"

namespace {

using namespace gdl;
using Catch::Matchers::WithinAbs;

constexpr f64 kEpsilon = 1e-5;

TEST_CASE("streams are summed into a zeroed buffer", "[audio][mixer]") {
    AudioMixer mixer(48000);
    REQUIRE(mixer.outputRate() == 48000);
    auto a = mixer.createStream(AudioStreamDesc{48000, 1});
    auto b = mixer.createStream(AudioStreamDesc{48000, 1});
    REQUIRE(mixer.streamCount() == 2);
    const std::array<f32, 2> kA{0.25f, 0.25f};
    const std::array<f32, 2> kB{0.5f, 0.5f};
    a->push(kA);
    b->push(kB);

    std::vector<f32> out{9.0f, 9.0f, 9.0f, 9.0f};
    mixer.mix(out);
    CHECK_THAT(out[0], WithinAbs(0.75, kEpsilon));
    CHECK_THAT(out[3], WithinAbs(0.75, kEpsilon));
}

TEST_CASE("finished streams are dropped after they drain", "[audio][mixer]") {
    AudioMixer mixer(48000);
    auto stream = mixer.createStream(AudioStreamDesc{48000, 1});
    const std::array<f32, 1> kInput{1.0f};
    stream->push(kInput);
    stream->finish();
    std::vector<f32> out(4, 0.0f);
    mixer.mix(out);
    REQUIRE(mixer.streamCount() == 0);
    REQUIRE(stream->drained());
}

TEST_CASE("abandoned empty streams are dropped", "[audio][mixer]") {
    AudioMixer mixer(48000);
    mixer.createStream(AudioStreamDesc{48000, 1});
    REQUIRE(mixer.streamCount() == 1);
    std::vector<f32> out(4, 0.0f);
    mixer.mix(out);
    REQUIRE(mixer.streamCount() == 0);
}

} // namespace
