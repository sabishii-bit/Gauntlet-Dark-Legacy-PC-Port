#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/platform/DisplayTiming.h"

namespace {
using namespace gdl;

TEST_CASE("render limits never exceed the active monitor even with V-Sync disabled",
          "[platform][display][graphics]") {
    for (const u32 monitor : {30U, 50U, 60U, 120U, 144U, 240U}) {
        CAPTURE(monitor);
        CHECK(displayFrameRate(0, monitor) == monitor);
        CHECK(displayFrameRate(30, monitor) == 30);
        CHECK(displayFrameRate(60, monitor) <= monitor);
        CHECK(displayFrameRate(120, monitor) <= monitor);
        CHECK(displayFrameRate(1000, monitor) == monitor);
    }
    CHECK(displayFrameRate(60, 144) == 60);
    CHECK(displayFrameRate(60, 50) == 50);
    CHECK(displayFrameRate(0, 0) == 60);
    CHECK(displayFrameRate(120, 0) == 60);
}

TEST_CASE("monitor refresh follows largest window overlap including negative desktop coordinates",
          "[platform][display][graphics]") {
    const DisplayBounds primary{0, 0, 1920, 1080};
    const DisplayBounds left{-2560, 0, 2560, 1440};
    const DisplayBounds above{0, -1080, 1920, 1080};
    const auto refresh = [&](DisplayBounds window) {
        DisplayRefresh selection(window);
        selection.consider(primary, 60);
        selection.consider(left, 144);
        selection.consider(above, 120);
        return selection.rate();
    };
    CHECK(refresh({100, 100, 1280, 720}) == 60);
    CHECK(refresh({-1280, 100, 1280, 720}) == 144);
    CHECK(refresh({100, -900, 1280, 720}) == 120);
    CHECK(refresh({-800, 100, 1280, 720}) == 144);
    CHECK(refresh({-480, 100, 1280, 720}) == 60);
    CHECK(refresh({-640, 100, 1280, 720}) == 60); // equal areas keep primary
    CHECK(refresh({5000, 0, 100, 100}) == 60);    // off-desktop keeps primary
}

TEST_CASE("absent invalid and changed monitor modes have a safe refresh fallback",
          "[platform][display][graphics]") {
    DisplayRefresh absent({0, 0, 1280, 720});
    CHECK(absent.rate() == 60);
    absent.consider({0, 0, 0, 1080}, 144);
    CHECK(absent.rate() == 60);
    absent.consider({0, 0, 1920, 1080}, 0);
    CHECK(absent.rate() == 60);
    for (const u32 mode : {60U, 144U, 60U}) {
        DisplayRefresh changed({0, 0, 1280, 720});
        changed.consider({0, 0, 1920, 1080}, mode);
        CHECK(changed.rate() == mode);
    }
}

TEST_CASE("absolute pacing absorbs scheduler oversleep without reducing the sustained cap",
          "[platform][display][graphics]") {
    using namespace std::chrono_literals;
    FramePacer pacer;
    const FramePacer::Time start{};
    const auto period = std::chrono::duration_cast<FramePacer::Clock::duration>(
        std::chrono::duration<f64>(1.0 / 60));
    auto deadline = pacer.deadline(start, start + 3ms, 60);
    CHECK(deadline == start + period);
    for (u32 frame = 1; frame < 600; ++frame) {
        // Simulate a consistently late wakeup without sleeping or opening a GPU window.
        const auto frameStart = deadline + 3ms;
        deadline = pacer.deadline(frameStart, frameStart + 3ms, 60);
        CHECK(deadline == start + period * (frame + 1));
        CHECK(deadline - frameStart >= period - period / 4);
    }
}

TEST_CASE("pacing bounds correction and resets after stalls or changes of monitor cap",
          "[platform][display][graphics]") {
    using namespace std::chrono_literals;
    FramePacer pacer;
    const FramePacer::Time start{};
    const auto period = std::chrono::duration_cast<FramePacer::Clock::duration>(
        std::chrono::duration<f64>(1.0 / 60));
    auto deadline = pacer.deadline(start, start + 1ms, 60);
    const auto lateStart = deadline + 5ms;
    deadline = pacer.deadline(lateStart, lateStart + 1ms, 60);
    CHECK(deadline == lateStart + period - period / 4);
    const auto stalledStart = deadline + 100ms;
    deadline = pacer.deadline(stalledStart, stalledStart + 1ms, 60);
    CHECK(deadline == stalledStart + period);
    const auto slowStart = deadline;
    const auto slowFinish = slowStart + 80ms;
    CHECK(pacer.deadline(slowStart, slowFinish, 60) == slowFinish);
    CHECK(pacer.deadline(slowFinish, slowFinish + 1ms, 60) == slowFinish + period);
    const auto changedStart = slowFinish + period;
    const auto newPeriod = std::chrono::duration_cast<FramePacer::Clock::duration>(
        std::chrono::duration<f64>(1.0 / 30));
    CHECK(pacer.deadline(changedStart, changedStart + 1ms, 30) == changedStart + newPeriod);
    CHECK(pacer.deadline(changedStart, changedStart + 1ms, 0) == changedStart + period);
}
} // namespace
