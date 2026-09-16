#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Clock.h"

namespace {

using namespace gdl;

TEST_CASE("a new clock has not ticked", "[core][clock]") {
    const FrameClock clock;
    REQUIRE(clock.frameIndex() == 0);
    REQUIRE(clock.deltaSeconds() == 0.0);
    REQUIRE(clock.totalSeconds() == 0.0);
}

TEST_CASE("tick advances the frame index and reports non-negative time", "[core][clock]") {
    FrameClock clock;
    clock.tick();
    clock.tick();
    REQUIRE(clock.frameIndex() == 2);
    REQUIRE(clock.deltaSeconds() >= 0.0);
    REQUIRE(clock.totalSeconds() >= clock.deltaSeconds());
}

TEST_CASE("delta time is clamped to the configured maximum", "[core][clock]") {
    FrameClock clock;
    clock.setMaxDeltaSeconds(0.001);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    clock.tick();
    REQUIRE(clock.deltaSeconds() == 0.001);
    REQUIRE(clock.totalSeconds() >= 0.01);
}

} // namespace
