#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/bosses/BossDefinition.h"
#include "game/level/BossKinds.h"

namespace {

using namespace gdl;
using namespace gdl::game;

constexpr s32 kLowest = std::numeric_limits<s32>::min();

TEST_CASE("the spider's and the lich's codes pick their own generator art and every other "
          "code BOSSGEN",
          "[game][bosses][boss-kinds]") {
    struct Row {
        s32 code;
        std::string_view tree;
        bool settled;
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, "BOSSGEN", false},
        {33, "BOSSGEN", false},
        {34, "BOSSGEN", false},
        {35, "BOSSGEN", false},
        {36, "BOSSGEN", false},
        {37, "GENPROJHIT", true},
        {38, "BOSSGEN", false},
        {39, "BOSSGEN", false},
        {40, "BOSSGEN", false},
        {41, "ATK12GEN", false},
        {42, "BOSSGEN", false},
        {43, "BOSSGEN", false},
        {44, "BOSSGEN", false},
        {45, "BOSSGEN", false},
        {kLowest, "BOSSGEN", false},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        const BossGeneratorVisual visual = bossGeneratorVisual(row.code);
        CHECK(visual.tree == row.tree);
        CHECK(visual.settled == row.settled);
    }
}

TEST_CASE("only the yeti's code names a boss whose projectiles end with a visual",
          "[game][bosses][boss-kinds]") {
    struct Row {
        s32 code;
        bool endVisual;
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, false},
        {33, false},
        {34, false},
        {35, false},
        {36, false},
        {37, false},
        {38, false},
        {39, true},
        {40, false},
        {41, false},
        {42, false},
        {43, false},
        {44, false},
        {45, false},
        {kLowest, false},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        CHECK(bossDefinition(bossNameOf(row.code)).projectileEndVisual == row.endVisual);
    }
}

TEST_CASE("each shipped boss code is fought on one level that lists it and has its critter data",
          "[bosses][boss-kinds][unpacked]") {
    const auto realms = test::unpackedOrSkip("wdata");
    const auto critters = test::unpackedOrSkip("critter");
    constexpr s32 kFirstBoss = 34; ///< the dragon
    constexpr s32 kLastBoss = 44;  ///< the garm
    std::map<s32, usize> fought;
    for (const auto& entry : std::filesystem::directory_iterator(realms)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        WorldData data;
        REQUIRE(data.load(entry.path()));
        for (const LevelInfo& level : data.levels()) {
            CAPTURE(entry.path().filename().string(), level.name, level.bossType);
            if (level.bossType == -1) {
                continue;
            }
            REQUIRE(level.bossType >= kFirstBoss);
            REQUIRE(level.bossType <= kLastBoss);
            ++fought[level.bossType];
            CHECK(std::ranges::find(level.enemies, level.bossType, &LevelEnemy::kind) !=
                  level.enemies.end());
            CHECK(std::filesystem::exists(critters /
                                          (std::string(bossNameOf(level.bossType)) + ".json")));
        }
    }
    for (s32 code = kFirstBoss; code <= kLastBoss; ++code) {
        CAPTURE(code);
        CHECK(fought[code] == 1U);
    }
}

} // namespace
