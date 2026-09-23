#include <cmath>
#include <cstddef>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AdsStream.h"
#include "engine/io/File.h"

#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

TEST_CASE("the tower's music stream decodes piece by piece and rewinds",
          "[audio][stream][assets]") {
    const auto path = test::assetOrSkip("STREAMS/tower.ads");
    AdsStream stream;
    REQUIRE_FALSE(stream.opened());
    REQUIRE(stream.open(path));
    REQUIRE(stream.opened());
    REQUIRE(stream.info().sampleRate == 44100);
    REQUIRE(stream.info().channels == 2);
    REQUIRE(stream.desc().sampleRate == 44100);
    REQUIRE(stream.desc().channels == 2);
    REQUIRE(stream.seconds() > 60.0);

    // Half a second at a time, interleaved stereo, until the stream runs out.
    std::vector<float> out;
    REQUIRE(stream.read(out, 22050));
    REQUIRE(out.size() % 2 == 0);
    REQUIRE(out.size() > std::size_t{2} * 1000);
    bool loud = false;
    std::size_t frames = out.size() / 2;
    while (stream.read(out, 22050)) {
        for (const float sample : out) {
            loud = loud || std::abs(sample) > 0.1f;
        }
        frames += out.size() / 2;
        out.clear();
    }
    REQUIRE(loud);
    REQUIRE(static_cast<double>(frames) == Approx(stream.info().sampleCount).epsilon(0.01));
    REQUIRE_FALSE(stream.read(out, 22050)); // exhausted stays exhausted

    stream.rewind();
    out.clear();
    REQUIRE(stream.read(out, 22050));
    REQUIRE(out.size() > std::size_t{2} * 1000);
}

TEST_CASE("a missing or foreign file is not a stream", "[audio][stream]") {
    AdsStream stream;
    REQUIRE_FALSE(stream.open(test::scratchDirectory("ads-stream-none") / "none.ads"));
    REQUIRE_FALSE(stream.opened());
    std::vector<float> out;
    REQUIRE_FALSE(stream.read(out, 100));
    REQUIRE(stream.seconds() == 0.0);
    const auto dir = test::scratchDirectory("ads-stream-bad");
    writeTextFile(dir / "bad.ads", "this is not audio at all, just some text long enough");
    REQUIRE_FALSE(stream.open(dir / "bad.ads"));
}

} // namespace
