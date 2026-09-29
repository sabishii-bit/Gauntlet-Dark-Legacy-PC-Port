#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/ExitWait.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("one waiting on an exit is named at ten seconds and every nine after",
          "[game][players][exit-wait]") {
    ExitWait wait;
    s32 spoken = 0;
    s32 first = -1;
    s32 second = -1;
    for (s32 tick = 2; tick <= 1200; tick += 2) {
        if (wait.step(true, 2)) {
            ++spoken;
            (first < 0 ? first : second) = tick;
        }
    }
    CHECK(first == 600);
    CHECK(second == 1140);
    CHECK(spoken == 2);
    // Moving off, or away, starts it over.
    CHECK_FALSE(wait.step(false, 2));
    CHECK(wait.ticks() == 0);
    CHECK_FALSE(wait.step(true, 598));
    CHECK(wait.step(true, 2));
}

} // namespace
