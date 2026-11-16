#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "expansions/shared/hud/LegendsHudClock.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("the painter clock counts thirtieths of the play clock and snaps when it runs back",
          "[legends][hud-clock]") {
    CHECK(LegendsHudClock::kStepsPerSecond == 30.0f);
    LegendsHudClock clock;

    // The first sight of the play clock only sets where the clock stands.
    CHECK(clock.advance(10.0f) == 0);
    CHECK(clock.step() == 300);
    CHECK_FALSE(clock.rewound());

    // From there it counts the whole thirtieths passed, and none while play stands still.
    CHECK(clock.advance(10.25f) == 7); // 307.5 steps in
    CHECK(clock.step() == 307);
    CHECK(clock.advance(10.5f) == 8);
    CHECK(clock.advance(10.5f) == 0);
    CHECK(clock.step() == 315);
    CHECK_FALSE(clock.rewound());

    // A rewind takes it back with the play clock, counting nothing, and the next advance counts
    // on from there.
    CHECK(clock.advance(9.0f) == 0);
    CHECK(clock.rewound());
    CHECK(clock.step() == 270);
    CHECK(clock.advance(9.5f) == 15);
    CHECK_FALSE(clock.rewound());

    // However far play ran, one advance reports a second of steps at most, and stands where
    // play does.
    CHECK(clock.advance(100.0f) == LegendsHudClock::kMostCatchUp);
    CHECK(LegendsHudClock::kMostCatchUp == 30);
    CHECK(clock.step() == 3000);

    // A play clock that is no number leaves it where it stood.
    CHECK(clock.advance(std::numeric_limits<f32>::quiet_NaN()) == 0);
    CHECK(clock.step() == 3000);
    CHECK_FALSE(clock.rewound());
}

} // namespace
