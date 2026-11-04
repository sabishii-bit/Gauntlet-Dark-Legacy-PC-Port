#include <array>
#include <limits>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/level/BossKinds.h"

namespace {

using namespace gdl;
using namespace gdl::game;

constexpr s32 kLowest = std::numeric_limits<s32>::min();
constexpr std::array<s32, 4> kNoBoss{-1, 33, 45, kLowest};

TEST_CASE("every boss code keeps its name and no other code has one", "[game][level][boss-kinds]") {
    struct Row {
        s32 code;
        std::string_view name;
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, ""},
        {33, ""},
        {34, "DRAGON"},
        {35, "CHIMERA"},
        {36, "DJINN"},
        {37, "DRIDER"},
        {38, "PBOSS"},
        {39, "YETI"},
        {40, "WRAITH"},
        {41, "LICH"},
        {42, "SKORNE1"},
        {43, "SKORNE2"},
        {44, "GARM"},
        {45, ""},
        {kLowest, ""},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        CHECK(bossNameOf(row.code) == row.name);
    }
}

TEST_CASE("only the lich's and the garm's codes hide an arena mesh", "[game][level][boss-kinds]") {
    struct Row {
        s32 code;
        std::string_view object;
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, ""},
        {33, ""},
        {34, ""},
        {35, ""},
        {36, ""},
        {37, ""},
        {38, ""},
        {39, ""},
        {40, ""},
        {41, "G5BIGDIRT"},
        {42, ""},
        {43, ""},
        {44, "H4NSFFXL_PURPLE"},
        {45, ""},
        {kLowest, ""},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        CHECK(bossArenaObject(row.code) == row.object);
    }
}

TEST_CASE("each boss type names its own boss and spans the dragon to the garm",
          "[game][level][boss-kinds]") {
    struct Row {
        s32 code;
        std::string_view name;
    };
    constexpr std::array<Row, 11> kRows{{
        {boss_type::kDragon, "DRAGON"},
        {boss_type::kChimera, "CHIMERA"},
        {boss_type::kDjinn, "DJINN"},
        {boss_type::kDrider, "DRIDER"},
        {boss_type::kPlagueBoss, "PBOSS"},
        {boss_type::kYeti, "YETI"},
        {boss_type::kWraith, "WRAITH"},
        {boss_type::kLich, "LICH"},
        {boss_type::kSkorne1, "SKORNE1"},
        {boss_type::kSkorne2, "SKORNE2"},
        {boss_type::kGarm, "GARM"},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        CHECK(bossNameOf(row.code) == row.name);
    }
    STATIC_CHECK(kFirstBossType == 34);
    STATIC_CHECK(kLastBossType == 44);
    STATIC_CHECK(kBossTypeCount == kRows.size());
}

TEST_CASE("a code outside the bosses is no boss type and has no stage",
          "[game][level][boss-kinds]") {
    for (const s32 code : kNoBoss) {
        CAPTURE(code);
        CHECK_FALSE(isBossType(code));
        CHECK(bossStageOf(code) == nullptr);
    }
}

TEST_CASE("each boss code's stage is its own and is found again by its name",
          "[game][level][boss-kinds]") {
    for (s32 code = kFirstBossType; code <= kLastBossType; ++code) {
        CAPTURE(code);
        CHECK(isBossType(code));
        const BossStage* stage = bossStageOf(code);
        REQUIRE(stage != nullptr);
        CHECK(stage->kind == code);
        CHECK(stage->name == bossNameOf(code));
        CHECK(bossStageNamed(bossNameOf(code)) == stage);
    }
    CHECK(bossStageNamed("") == nullptr);
    CHECK(bossStageNamed("GRUNT") == nullptr);
}

TEST_CASE("the boss stages say what each level does about its boss", "[game][level][boss-kinds]") {
    struct Row {
        s32 code;
        std::string_view fireHoleSound;
        char swarmBankVariant;
        bool fallingMasonry;
        bool roams;
    };
    constexpr std::array<Row, 11> kRows{{
        {34, "S_FIREHOLE2", '\0', false, false},
        {35, "", '\0', false, false},
        {36, "", 'C', false, false},
        {37, "", 'D', false, true},
        {38, "", '\0', false, false},
        {39, "", '\0', false, false},
        {40, "", '\0', false, false},
        {41, "", 'B', false, true},
        {42, "", '\0', true, false},
        {43, "", '\0', false, false},
        {44, "", '\0', false, false},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        const BossStage* stage = bossStageOf(row.code);
        REQUIRE(stage != nullptr);
        CHECK(stage->fireHoleSound == row.fireHoleSound);
        CHECK(stage->swarmBankVariant == row.swarmBankVariant);
        CHECK(stage->fallingMasonry == row.fallingMasonry);
        CHECK(stage->roams == row.roams);
    }
}

TEST_CASE("only the final bosses' falls end the game and only the garm summons his brood",
          "[game][level][boss-kinds]") {
    struct Row {
        s32 code;
        bool summonsBrood;
        std::string_view movie; ///< empty: no finale
        bool completesGame;
    };
    constexpr std::array<Row, kBossTypeCount> kRows{{
        {34, false, "", false},
        {35, false, "", false},
        {36, false, "", false},
        {37, false, "", false},
        {38, false, "", false},
        {39, false, "", false},
        {40, false, "", false},
        {41, false, "", false},
        {42, false, "", false},
        {43, false, "garm", false},
        {44, true, "victory", true},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        const BossStage* stage = bossStageOf(row.code);
        REQUIRE(stage != nullptr);
        CHECK(stage->summonsBrood == row.summonsBrood);
        REQUIRE(stage->finale.has_value() == !row.movie.empty());
        if (stage->finale.has_value()) {
            CHECK(stage->finale->movie == row.movie);
            CHECK(stage->finale->completesGame == row.completesGame);
        }
    }
}

} // namespace
