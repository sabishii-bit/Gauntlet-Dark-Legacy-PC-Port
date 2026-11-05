#include <catch2/catch_test_macros.hpp>

#include "game/characters/LevelWatch.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("a level watch reports each player's changes of level, and which cross a tier",
          "[game][players]") {
    LevelWatch watch;
    // The first sighting only sets the mark.
    REQUIRE_FALSE(watch.observe(0, 1).has_value());
    REQUIRE(watch.markOf(0) == 1);
    REQUIRE_FALSE(watch.markOf(1).has_value());
    REQUIRE_FALSE(watch.observe(0, 1).has_value());
    // A level gained is reported once, from and to.
    auto change = watch.observe(0, 2);
    REQUIRE(change.has_value());
    REQUIRE(change->player == 0);
    REQUIRE(change->from == 1);
    REQUIRE(change->to == 2);
    REQUIRE(change->gained());
    REQUIRE_FALSE(change->milestone());
    REQUIRE_FALSE(watch.observe(0, 2).has_value());
    // Several at once are one change; the tenth crosses a tier.
    change = watch.observe(0, 11);
    REQUIRE(change.has_value());
    REQUIRE(change->from == 2);
    REQUIRE(change->to == 11);
    REQUIRE(change->milestone());
    REQUIRE_FALSE(watch.observe(0, 12)->milestone());
    REQUIRE(watch.observe(0, 20)->milestone());
    // A level lost (Death's draining) is a change too, but no gain.
    change = watch.observe(0, 19);
    REQUIRE(change.has_value());
    REQUIRE_FALSE(change->gained());
    REQUIRE(change->milestone());
    // Players are watched apart, and can be forgotten.
    REQUIRE_FALSE(watch.observe(2, 5).has_value());
    REQUIRE(watch.observe(2, 6)->player == 2);
    watch.forget(2);
    REQUIRE_FALSE(watch.observe(2, 9).has_value()); // a fresh mark
    watch.clear();
    REQUIRE_FALSE(watch.markOf(0).has_value());
}

TEST_CASE("level watches rewind by copy with their pending level changes",
          "[game][players][rollback]") {
    LevelWatch native;
    REQUIRE_FALSE(native.observe(3, 9));
    REQUIRE_FALSE(native.observe(0, 14));
    const LevelWatch saved = native;
    LevelWatch replay = saved;
    CHECK(replay == saved);
    const auto gained = replay.observe(3, 10);
    REQUIRE(gained);
    CHECK(gained->from == 9);
    CHECK(gained->to == 10);
    CHECK(gained->milestone());
    CHECK_FALSE(replay.observe(3, 10));
    replay = saved;
    CHECK(replay == saved);
    native.forget(3);
    replay = native;
    CHECK_FALSE(replay.markOf(3));
    CHECK_FALSE(replay.observe(3, 10));
}

} // namespace
