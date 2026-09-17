#include <catch2/catch_test_macros.hpp>

#include "game/players/Progression.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("the experience curve matches the original's levels", "[game][players]") {
    REQUIRE(levelExperience(1) == 0);
    REQUIRE(levelExperience(2) == 1060);
    REQUIRE(levelExperience(60) == 165200);
    REQUIRE(levelExperience(61) == 165200 + 4600);
    REQUIRE(experienceLevel(0) == 1);
    REQUIRE(experienceLevel(1059) == 1);
    REQUIRE(experienceLevel(1060) == 2);
    REQUIRE(experienceLevel(165200) == 60);
    REQUIRE(experienceLevel(10'000'000) == kMaxLevel);
}

TEST_CASE("displayed stats grow with the level and the saved bonuses", "[game][players]") {
    ClassStats warrior;
    warrior.fightMin = 600.0f;
    warrior.speedMin = 350.0f;
    warrior.armorMin = 300.0f;
    warrior.magicMin = 100.0f;
    const StatBlock fresh = displayStats(warrior, 1, ClassProgress{});
    REQUIRE(fresh.strength() == 600);
    REQUIRE(fresh.speed() == 350);
    REQUIRE(fresh.armor() == 300);
    REQUIRE(fresh.magic() == 100);
    REQUIRE(fresh.best() == 0);

    ClassProgress progress;
    progress.magicAdd = 20.0f;
    const StatBlock later = displayStats(warrior, 3, progress);
    REQUIRE(later.strength() == 610);
    REQUIRE(later.magic() == 130);

    ClassProgress huge;
    huge.fightAdd = 5000.0f;
    REQUIRE(displayStats(warrior, 99, huge).strength() == kMaxStat);

    const StatBlock master = masteryStats();
    REQUIRE(master.magic() == kMaxStat);
    REQUIRE(master.best() == 0);
}

} // namespace
