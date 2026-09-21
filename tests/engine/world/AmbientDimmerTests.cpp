#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/world/AmbientDimmer.h"

namespace {

using namespace gdl;
using Catch::Approx;

TEST_CASE("the light falls fast while the dark is asked for and comes back slowly after",
          "[world][lighting]") {
    AmbientDimmer dimmer;
    REQUIRE(dimmer.offset() == 0.0f);
    REQUIRE(dimmer.applied(0.7f) == Approx(0.7f));
    // Asked for every frame, it falls a quarter a frame to what is asked and stays.
    const f32 frame = AmbientDimmer::kFrameSeconds;
    dimmer.ask(-0.6f);
    dimmer.update(frame);
    REQUIRE(dimmer.offset() == Approx(-0.25f));
    dimmer.ask(-0.6f);
    dimmer.update(frame);
    dimmer.ask(-0.6f);
    dimmer.update(frame);
    REQUIRE(dimmer.offset() == Approx(-0.6f));
    REQUIRE(dimmer.applied(0.7f) == Approx(0.1f));
    REQUIRE(dimmer.applied(0.3f) == 0.0f); // never under none
    for (int i = 0; i < 30; ++i) {
        dimmer.ask(-0.6f);
        dimmer.update(frame);
    }
    REQUIRE(dimmer.offset() == Approx(-0.6f));
    // Left alone, what was asked fades and the light climbs back a twentieth a frame.
    dimmer.update(3.0f * frame);
    const f32 rising = dimmer.offset();
    REQUIRE(rising > -0.6f);
    REQUIRE(rising < -0.4f);
    dimmer.update(1.0f);
    REQUIRE(dimmer.offset() == 0.0f);
    // A frame at sixty a second is half a step of it.
    dimmer.reset();
    dimmer.ask(-0.4f);
    dimmer.update(frame * 0.5f);
    REQUIRE(dimmer.offset() == 0.0f);
    dimmer.ask(-0.4f);
    dimmer.update(frame * 0.5f);
    REQUIRE(dimmer.offset() == Approx(-0.25f));
    dimmer.reset();
    REQUIRE(dimmer.offset() == 0.0f);
}

} // namespace
