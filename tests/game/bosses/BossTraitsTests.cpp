#include <array>
#include <limits>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/bosses/BossTraits.h"
#include "game/level/BossKinds.h"

namespace {

using namespace gdl;
using namespace gdl::game;

constexpr s32 kLowest = std::numeric_limits<s32>::min();
constexpr std::array<s32, 4> kNoBoss{-1, 33, 45, kLowest};

TEST_CASE("each boss code's traits sit at its own row", "[game][bosses][boss-traits]") {
    for (s32 code = kFirstBossType; code <= kLastBossType; ++code) {
        CAPTURE(code);
        const BossTraits* traits = bossTraitsOf(code);
        REQUIRE(traits != nullptr);
        CHECK(traits->kind == code);
        CHECK(&legendStagingOf(code) == &traits->legend);
    }
}

TEST_CASE("a code outside the bosses has no traits and the default legend staging",
          "[game][bosses][boss-traits]") {
    const LegendStaging defaults{};
    for (const s32 code : kNoBoss) {
        CAPTURE(code);
        CHECK(bossTraitsOf(code) == nullptr);
        CHECK(legendStagingOf(code) == defaults);
    }
}

TEST_CASE("only the Chimera's rite holds the party to a crawl, two a second along each axis",
          "[game][bosses][boss-traits][legend]") {
    for (s32 code = kFirstBossType; code <= kLastBossType; ++code) {
        CAPTURE(code);
        if (code == boss_type::kChimera) {
            CHECK(legendStagingOf(code).partyPace == 2.0f); // PlayerMotion's 0x80347B98
        } else {
            CHECK_FALSE(legendStagingOf(code).partyPace.has_value());
        }
    }
    for (const s32 code : kNoBoss) {
        CAPTURE(code);
        CHECK_FALSE(legendStagingOf(code).partyPace.has_value());
    }
}

TEST_CASE("each boss's defeat says its own lines and sends its own wizard",
          "[game][bosses][boss-traits]") {
    struct Row {
        s32 code;
        std::string_view speech;
        std::string_view defeatVoice;
        RuneVoices runeVoices;
        VictoryFollowUp followUp;
        bool longWizardWait;
        bool wizardAtAltar;
        bool dropsKey;
        BossSpew spew;
    };
    using enum RuneVoices;
    using enum VictoryFollowUp;
    using enum BossSpew;
    constexpr std::array<Row, kBossTypeCount> kRows{{
        {34, "DRAGON_SPEECH", "", SomeIsAll, Realm, false, false, true, Coins},
        {35, "CHIMERA_SPEECH", "", SomeIsAll, Realm, false, false, true, Coins},
        {36, "DJINN_SPEECH", "", SomeIsAll, Realm, false, false, true, Coins},
        {37, "DRIDER_SPEECH", "", SomeIsAll, Realm, false, false, true, Coins},
        {38, "PBOSS_SPEECH", "", OwnAll, Realm, false, false, true, Coins},
        {39, "YETI_SPEECH", "", OwnAll, Realm, false, false, true, Coins},
        {40, "WRAITH_SPEECH", "", OwnAll, Realm, false, false, true, Coins},
        {41, "LICH_SPEECH", "", OwnAll, Realm, false, false, true, Coins},
        {42, "SKORNE1_SPEECH", "S_E2VOXA", Temple, TempleRunes, true, true, false, Relics},
        {43, "SKORNE2_SPEECH", "S_ENDVOX", None, Leaves, false, true, false, Coins},
        {44, "GARM2_SPEECH", "S_GRMDESTVOX", None, Leaves, true, false, false, Coins},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        const BossTraits* traits = bossTraitsOf(row.code);
        REQUIRE(traits != nullptr);
        const BossDefeat& defeat = traits->defeat;
        CHECK(defeat.speech == row.speech);
        CHECK(defeat.defeatVoice == row.defeatVoice);
        CHECK(defeat.runeVoices == row.runeVoices);
        CHECK(defeat.followUp == row.followUp);
        CHECK(defeat.longWizardWait == row.longWizardWait);
        CHECK(defeat.wizardAtAltar == row.wizardAtAltar);
        CHECK(defeat.dropsKey == row.dropsKey);
        CHECK(defeat.spew == row.spew);
    }
}

TEST_CASE("a code outside the bosses reads the nearest boss's row where the rules are ordered",
          "[game][bosses][boss-traits]") {
    for (s32 code = kFirstBossType; code <= kLastBossType; ++code) {
        CAPTURE(code);
        CHECK(&orderedBossTraitsOf(code) == bossTraitsOf(code));
    }
    CHECK(orderedBossTraitsOf(-1).kind == 34);
    CHECK(orderedBossTraitsOf(33).kind == 34);
    CHECK(orderedBossTraitsOf(kLowest).kind == 34);
    CHECK(orderedBossTraitsOf(45).kind == 44);
    CHECK(orderedBossTraitsOf(std::numeric_limits<s32>::max()).kind == 44);
}

} // namespace
