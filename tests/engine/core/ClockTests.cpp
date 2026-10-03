#include <chrono>
#include <limits>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Clock.h"

namespace {

using namespace gdl;

TEST_CASE("simulation time stays at sixty ticks across render caps and uncapped frames",
          "[core][clock][graphics]") {
    for (const u32 rate : {30U, 60U, 144U, 240U, 1000U}) {
        CAPTURE(rate);
        UpdateClock clock;
        u32 ticks = 0;
        for (u32 frame = 0; frame < rate * 10; ++frame) {
            ticks += clock.advance(1.0 / rate, 60);
        }
        REQUIRE(ticks == 600);
    }
}

TEST_CASE("simulation clock retains fractions through rate changes and bounds stalls",
          "[core][clock][graphics]") {
    UpdateClock clock;
    CHECK(clock.advance(1.0 / 240, 60) == 0);
    CHECK(clock.advance(1.0 / 240, 60) == 0);
    CHECK(clock.advance(1.0 / 120, 60) == 1);
    CHECK(clock.advance(1.0 / 30, 60) == 2);
    CHECK(clock.advance(0, 60) == 0);
    CHECK(clock.advance(-1, 60) == 0);
    CHECK(clock.advance(std::numeric_limits<f64>::infinity(), 60) == 0);
    CHECK(clock.advance(1, 0) == 0);
    CHECK(clock.advance(10, 60) == 15);
    CHECK(clock.advance(0, 60) == 0);
}

TEST_CASE("fixed simulation steps keep nonlinear motion identical across render cadences",
          "[core][clock][graphics]") {
    const auto simulate = [](u32 renderRate) {
        UpdateClock clock;
        f64 velocity = 0;
        f64 position = 0;
        for (u32 frame = 0; frame < renderRate * 5; ++frame) {
            const u32 ticks = clock.advance(1.0 / renderRate, 60);
            for (u32 tick = 0; tick < ticks; ++tick) {
                velocity += (3.0 - velocity * 0.2) / 60;
                position += velocity / 60;
            }
        }
        return position;
    };
    const f64 reference = simulate(60);
    for (const u32 rate : {24U, 30U, 60U, 144U, 240U, 1000U}) {
        CAPTURE(rate);
        CHECK(simulate(rate) == reference);
    }
}

TEST_CASE("a new clock has not ticked", "[core][clock]") {
    const FrameClock clock;
    REQUIRE(clock.frameIndex() == 0);
    REQUIRE(clock.deltaSeconds() == 0.0);
    REQUIRE(clock.totalSeconds() == 0.0);
}

TEST_CASE("presentation advances between fixed updates without inventing ticks",
          "[core][clock][cadence]") {
    UpdateClock clock;
    CHECK(clock.advance(1.0 / 240, 60) == 0);
    CHECK(clock.fraction(60) == 0.25f);
    CHECK(clock.advance(1.0 / 240, 60) == 0);
    CHECK(clock.fraction(60) == 0.5f);
    CHECK(clock.advance(1.0 / 120, 60) == 1);
    CHECK(clock.fraction(60) == 0);
    CHECK(clock.advance(1.0 / 144, 60) == 0);
    CHECK(clock.fraction(60) > 0);
    CHECK(clock.fraction(60) < 1);
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
