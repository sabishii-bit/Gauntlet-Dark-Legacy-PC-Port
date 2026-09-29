#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/MeleeStreak.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a run of close blows earns the narrator's praise once it pauses",
          "[game][players][streak]") {
    MeleeStreak streak;
    CHECK(streak.step(5.0f).empty());
    // Twenty-nine hurts are not enough.
    for (s32 i = 0; i < 29; ++i) {
        streak.record(false);
        CHECK(streak.step(0.5f).empty());
    }
    CHECK(streak.step(2.1f).empty());
    CHECK(streak.count() == 0);
    // Thirty are heroic, but only once two seconds have passed without another.
    for (s32 i = 0; i < 30; ++i) {
        streak.record(false);
    }
    CHECK(streak.step(1.5f).empty());
    CHECK(streak.step(0.4f).empty());
    CHECK(streak.step(0.2f) == MeleeStreak::kHeroicVoice);
    CHECK(streak.count() == 0);
    // Kills count three: fifteen of them are bravery.
    for (s32 i = 0; i < 15; ++i) {
        streak.record(true);
    }
    CHECK(streak.count() == 45);
    CHECK(streak.step(2.5f) == MeleeStreak::kBraveryVoice);
    CHECK(streak.step(2.5f).empty());
}

} // namespace
