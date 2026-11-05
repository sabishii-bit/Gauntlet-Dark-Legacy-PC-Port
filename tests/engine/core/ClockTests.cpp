#include <chrono>
#include <limits>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Clock.h"
#include "engine/core/Types.h"

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

// Binary fractions of a second at 64 ticks a second keep every product exact.
constexpr f64 kBinaryRate = 64;
constexpr f64 kHalfTick = 0.0078125;     ///< half a tick at that rate, 1/128 s
constexpr f64 kQuarterTick = 0.00390625; ///< a quarter of a tick, 1/256 s
constexpr f64 kTolerance = 1.0e-9;       ///< a rounding tolerance

TEST_CASE("a tick accumulator carries the part of a tick an uneven update leaves",
          "[core][clock]") {
    TickAccumulator ticks;
    CHECK(ticks.take(3 * kHalfTick, kBinaryRate) == 1);
    CHECK(ticks.carry() == 0.5);
    CHECK(ticks.take(kQuarterTick, kBinaryRate) == 0);
    CHECK(ticks.carry() == 0.75);
    CHECK(ticks.take(kQuarterTick, kBinaryRate) == 1);
    CHECK(ticks.carry() == 0.0);
    CHECK(ticks.take(1.0, kBinaryRate) == 64);
    CHECK(ticks.carry() == 0.0);
}

TEST_CASE("a tick accumulator runs at most its limit and loses the ticks past it",
          "[core][clock]") {
    const TickAccumulator::Limits limits{.mostTicks = 6};
    TickAccumulator ticks{limits};
    CHECK(ticks.take(0.1, 120) == 6);
    CHECK(ticks.take(1.0 / 120 + 1.0e-6, 120) == 1);
}

TEST_CASE("a tick accumulator bounds what a stalled update brings", "[core][clock]") {
    const TickAccumulator::Limits limits{.longestUpdate = 0.25};
    TickAccumulator ticks{limits};
    CHECK(ticks.take(10.0, 60) == 15);
    CHECK(ticks.carry() == 0.0);
    CHECK(ticks.take(0.25, 60) == 15);
}

TEST_CASE("a tick accumulator's tolerance reaches a tick just short of whole", "[core][clock]") {
    constexpr f64 kJustShort = 1.0 - 1.0e-12;
    TickAccumulator exact;
    CHECK(exact.take(kJustShort, 1) == 0);
    CHECK(exact.carry() == kJustShort);
    const TickAccumulator::Limits limits{.tolerance = kTolerance};
    TickAccumulator tolerant{limits};
    CHECK(tolerant.take(kJustShort, 1) == 1);
}

TEST_CASE("a tick reached within the tolerance leaves no carry below zero", "[core][clock]") {
    const TickAccumulator::Limits limits{.tolerance = kTolerance};
    TickAccumulator ticks{limits};
    REQUIRE(ticks.take(1.0 - 1.0e-12, 1) == 1);
    CHECK(ticks.carry() == 0.0);
    CHECK(ticks.take(0.5, 1) == 0);
    CHECK(ticks.carry() == 0.5);
}

TEST_CASE("a tick accumulator takes nothing from a bad time or rate and keeps its carry",
          "[core][clock]") {
    constexpr f64 kInfinity = std::numeric_limits<f64>::infinity();
    constexpr f64 kNotANumber = std::numeric_limits<f64>::quiet_NaN();
    TickAccumulator ticks;
    REQUIRE(ticks.take(kHalfTick, kBinaryRate) == 0);
    REQUIRE(ticks.carry() == 0.5);
    CHECK(ticks.take(-1.0, kBinaryRate) == 0);
    CHECK(ticks.take(kInfinity, kBinaryRate) == 0);
    CHECK(ticks.take(-kInfinity, kBinaryRate) == 0);
    CHECK(ticks.take(kNotANumber, kBinaryRate) == 0);
    CHECK(ticks.take(1.0, 0) == 0);
    CHECK(ticks.take(1.0, -kBinaryRate) == 0);
    CHECK(ticks.take(1.0, kInfinity) == 0);
    CHECK(ticks.take(1.0, kNotANumber) == 0);
    CHECK(ticks.carry() == 0.5);
    CHECK(ticks.take(kHalfTick, kBinaryRate) == 1);
}

TEST_CASE("a reset tick accumulator starts from no carry and keeps its limits", "[core][clock]") {
    const TickAccumulator::Limits limits{.mostTicks = 1};
    TickAccumulator ticks{limits};
    REQUIRE(ticks.take(kHalfTick, kBinaryRate) == 0);
    ticks.reset();
    CHECK(ticks.carry() == 0.0);
    CHECK(ticks.take(kHalfTick, kBinaryRate) == 0);
    CHECK(ticks.carry() == 0.5);
    CHECK(ticks.take(1.0, kBinaryRate) == 1);
}

TEST_CASE("a tick accumulator brings the same ticks a second at every frame rate",
          "[core][clock]") {
    const TickAccumulator::Limits limits{.tolerance = kTolerance};
    for (const u32 frameRate : {30U, 60U, 120U, 144U, 240U}) {
        CAPTURE(frameRate);
        TickAccumulator ticks{limits};
        for (u32 second = 0; second < 10; ++second) {
            CAPTURE(second);
            s32 inSecond = 0;
            for (u32 frame = 0; frame < frameRate; ++frame) {
                inSecond += ticks.take(1.0 / frameRate, 60);
            }
            CHECK(inSecond == 60);
        }
    }
}

} // namespace
