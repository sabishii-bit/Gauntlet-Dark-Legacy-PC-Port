#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/LevelRecord.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("a level beaten sets its realm's bit and the runestone and legend passes",
          "[game][players][level-record]") {
    LevelRecord record;
    REQUIRE_FALSE(record.hasBeaten(7, 0));
    record.recordBeaten(7, 0, 8, 0); // G1 holds the eighth runestone
    REQUIRE(record.hasBeaten(7, 0));
    REQUIRE_FALSE(record.hasBeaten(7, 1));
    REQUIRE(record.beaten[7] == 0x01);
    REQUIRE(record.runeLevels == std::array<u16, 2>{1U << 7U, 0});
    REQUIRE(record.legendLevels == std::array<u16, 2>{0, 0});
    // Beaten again, the second pass is marked; the level's bit stays.
    record.recordBeaten(7, 0, 8, 0);
    REQUIRE(record.runeLevels == std::array<u16, 2>{1U << 7U, 1U << 7U});
    REQUIRE(record.beaten[7] == 0x01);
    // G3 holds the ice's legend item: a bit per realm of the item's boss.
    record.recordBeaten(7, 2, 0, 9);
    REQUIRE(record.beaten[7] == 0x05);
    REQUIRE(record.legendLevels == std::array<u16, 2>{1U << 9U, 0});
    record.recordBeaten(7, 2, 0, 9);
    REQUIRE(record.legendLevels == std::array<u16, 2>{1U << 9U, 1U << 9U});
}

TEST_CASE("a level out of the record's range is ignored", "[game][players][level-record]") {
    LevelRecord record;
    record.recordBeaten(-1, 0);
    record.recordBeaten(14, 0);
    record.recordBeaten(7, 8);
    record.recordBeaten(7, -1, 14, 16);
    REQUIRE(record == LevelRecord{});
    REQUIRE_FALSE(record.hasBeaten(14, 0));
    REQUIRE_FALSE(record.hasBeaten(7, 8));
}

TEST_CASE("a death on a boss level is a try, and another the second",
          "[game][players][level-record]") {
    LevelRecord record;
    record.recordBossDeath(2);
    REQUIRE(record.bossDeaths == std::array<u16, 2>{1U << 2U, 0});
    record.recordBossDeath(2);
    REQUIRE(record.bossDeaths == std::array<u16, 2>{1U << 2U, 1U << 2U});
    record.recordBossDeath(2);
    REQUIRE(record.bossDeaths == std::array<u16, 2>{1U << 2U, 1U << 2U});
    record.recordBossDeath(16);
    REQUIRE(record.bossDeaths == std::array<u16, 2>{1U << 2U, 1U << 2U});
}

} // namespace
