#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/players/EnemyShrink.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("every standing shrinker worn scales the swarm by two thirds again, never where a "
          "boss is fought",
          "[game][players][powerups][shrink]") {
    CHECK(EnemyShrink::scaleOf(0, false) == 1.0f);
    CHECK(EnemyShrink::scaleOf(1, false) == Approx(0.667f));
    CHECK(EnemyShrink::scaleOf(2, false) == Approx(0.667f * 0.667f));
    CHECK(EnemyShrink::scaleOf(4, false) == Approx(0.667f * 0.667f * 0.667f * 0.667f));
    CHECK(EnemyShrink::scaleOf(1, true) == 1.0f);
    CHECK(EnemyShrink::scaleOf(3, true) == 1.0f);
    CHECK_FALSE(EnemyShrink::shrunk(1.0f));
    CHECK(EnemyShrink::shrunk(0.667f));
}

TEST_CASE("the shrunk take double and deal half; whole they take and deal as they are",
          "[game][players][powerups][shrink]") {
    CHECK(EnemyShrink::harmTaken(0.667f, 17.0f) == Approx(34.0f));
    CHECK(EnemyShrink::harmDealt(0.667f, 10.0f) == Approx(5.0f));
    CHECK(EnemyShrink::harmTaken(1.0f, 17.0f) == 17.0f);
    CHECK(EnemyShrink::harmDealt(1.0f, 10.0f) == 10.0f);
}

TEST_CASE("the unshrink is heard as the scale rises back, not as it falls or holds",
          "[game][players][powerups][shrink]") {
    CHECK(EnemyShrink::rises(0.667f, 1.0f));
    CHECK(EnemyShrink::rises(0.667f * 0.667f, 0.667f));
    CHECK_FALSE(EnemyShrink::rises(1.0f, 0.667f));
    CHECK_FALSE(EnemyShrink::rises(1.0f, 1.0f));
    CHECK_FALSE(EnemyShrink::rises(0.667f, 0.667f));
    CHECK(EnemyShrink::kUnshrinkSound == "S_UNSHRINK");
}

} // namespace
