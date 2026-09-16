#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/codec/MoviePlayback.h"
#include "engine/math/Math.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

TEST_CASE("an unopened playback is inert", "[codec][movie]") {
    MoviePlayback playback;
    REQUIRE_FALSE(playback.isOpen());
    REQUIRE_FALSE(playback.update(0.1));
    REQUIRE_FALSE(playback.frameChanged());
    REQUIRE(playback.frame().pixels.empty());
    REQUIRE_FALSE(playback.open(test::scratchDirectory("movie-missing") / "none.avi"));
}

TEST_CASE("the Midway logo movie plays through", "[codec][movie][assets]") {
    const auto file = test::assetOrSkip("VQMOVIES/midway.avi");
    MoviePlayback playback;
    REQUIRE(playback.open(file));

    const MovieInfo& info = playback.info();
    REQUIRE(info.width == 512);
    REQUIRE(info.height == 384);
    CHECK_THAT(info.framesPerSecond, Catch::Matchers::WithinAbs(30.0, 0.01));
    REQUIRE(info.frameCount == 240);
    REQUIRE(info.hasAudio);
    REQUIRE(info.audioReady);
    REQUIRE(info.audioChannels == 2);
    REQUIRE(info.audioSampleRate == 48042);

    std::vector<f32> audio;
    bool sawColour = false;
    int updates = 0;
    while (playback.update(1.0 / 30.0) && updates < 1000) {
        ++updates;
        playback.takeAudio(audio);
        if (playback.frameChanged()) {
            const Image& frame = playback.frame();
            const Color centre = frame.pixel(frame.width / 2, frame.height / 2);
            sawColour = sawColour || centre != Color::black();
        }
    }
    playback.takeAudio(audio);

    REQUIRE(playback.decodedFrames() == 240);
    REQUIRE(sawColour);
    REQUIRE(audio.size() > 700000);
    REQUIRE(audio.size() < 800000);
    REQUIRE(std::ranges::max(audio) > 0.1f);
    REQUIRE(updates >= 240);
}

} // namespace
