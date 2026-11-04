#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/bosses/BossVictory.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Stage = BossVictory::Stage;

TEST_CASE("the wizard's words and voices go by the boss and the realm's runestones",
          "[game][screens][victory]") {
    REQUIRE(BossVictory::defeatMessageOf(41) == "LICH_SPEECH");
    REQUIRE(BossVictory::defeatMessageOf(34) == "DRAGON_SPEECH");
    REQUIRE(BossVictory::defeatMessageOf(44) == "GARM2_SPEECH");
    REQUIRE(BossVictory::defeatMessageOf(20).empty());
    REQUIRE(BossVictory::defeatVoiceOf(41, 'G') == "S_DEFEATVOXG");
    REQUIRE(BossVictory::defeatVoiceOf(42, 'E') == "S_E2VOXA");
    // The town has two runestones (the eighth and ninth): none, one, both.
    const u16 town = (1U << 7) | (1U << 8);
    REQUIRE(BossVictory::qualityOf(town, 0) == 0);
    REQUIRE(BossVictory::qualityOf(town, 1U << 7) == 1);
    REQUIRE(BossVictory::qualityOf(town, town | 1U) == 3);
    // The castle has one: none, or the one.
    const u16 castle = 1U << 0;
    REQUIRE(BossVictory::qualityOf(castle, 0) == 0);
    REQUIRE(BossVictory::qualityOf(castle, castle) == 2);
    REQUIRE(BossVictory::qualityOf(0, 0xFFF) == 0);
    REQUIRE(BossVictory::runeMessageOf(0) == "RUNE_PHRASE0");
    REQUIRE(BossVictory::runeMessageOf(1) == "RUNE_PHRASE1");
    REQUIRE(BossVictory::runeMessageOf(2) == "RUNE_PHRASE1B");
    REQUIRE(BossVictory::runeMessageOf(3) == "RUNE_PHRASE2");
    REQUIRE(BossVictory::runeVoiceOf(41, 'G', 0) == "S_RUNEVOX0G");
    REQUIRE(BossVictory::runeVoiceOf(41, 'G', 1) == "S_RUNEVOX1G");
    REQUIRE(BossVictory::runeVoiceOf(35, 'A', 2) == "S_RUNEVOX1A");
    REQUIRE(BossVictory::runeVoiceOf(41, 'G', 3) == "S_RUNEVOX2G");
    REQUIRE(BossVictory::runeVoiceOf(38, 'K', 2) == "S_RUNEVOX2K"); // the one of one
    REQUIRE(BossVictory::runeVoiceOf(34, 'B', 3) == "S_RUNEVOX1B"); // no line of its own
    REQUIRE(BossVictory::runeVoiceOf(42, 'E', 3) == "S_E2VOXB");
    REQUIRE(BossVictory::runeVoiceOf(44, 'H', 3).empty());
}

TEST_CASE("the wizard comes five seconds after the fall, fades in, types his two speeches "
          "out and sees the party off",
          "[game][screens][victory]") {
    BossVictory visit;
    REQUIRE(visit.stage() == Stage::None);
    REQUIRE_FALSE(visit.running());
    REQUIRE(visit.update(2, {}).empty());
    visit.begin(41, 'G', (1U << 7) | (1U << 8), 1U << 7, false);
    REQUIRE(visit.running());
    REQUIRE(visit.stage() == Stage::Waiting);
    REQUIRE(visit.runeQuality() == 1);
    REQUIRE_FALSE(visit.wizardShown());
    // Five seconds' wait, then he fades in over sixty-four ticks.
    for (s32 i = 0; i < BossVictory::kWaitTicks - 1; ++i) {
        REQUIRE(visit.update(1, {}).empty());
    }
    REQUIRE(visit.stage() == Stage::Waiting);
    visit.update(1, {});
    REQUIRE(visit.stage() == Stage::Appearing);
    REQUIRE(visit.wizardShown());
    REQUIRE(visit.wizardAlpha() == 0.0f);
    std::vector<VictoryVoice> voices;
    s32 fading = 0;
    while (visit.stage() == Stage::Appearing && fading < 200) {
        auto spoken = visit.update(1, {});
        voices.insert(voices.end(), spoken.begin(), spoken.end());
        ++fading;
    }
    REQUIRE(fading == 64);
    REQUIRE(visit.wizardAlpha() == 1.0f);
    // Then the defeat line, voiced from the level's bank, typed with weighted text units.
    REQUIRE(visit.stage() == Stage::Defeat);
    REQUIRE(voices.size() == 1);
    REQUIRE(voices[0].sound == "S_DEFEATVOXG");
    REQUIRE(visit.caption().has_value());
    REQUIRE(visit.caption()->message == "LICH_SPEECH");
    REQUIRE(visit.caption()->page == 0);
    REQUIRE(visit.caption()->shown == 0);
    const std::array<std::string, 2> pages{"ABCDEFGHIJ", "ABCD"};
    visit.update(2, pages);
    REQUIRE(visit.caption()->shown == 1);
    visit.update(34, pages);
    REQUIRE(visit.caption()->shown == 10);
    // A second's pause, and the next page; past the last, half a second, then the runes.
    visit.update(BossVictory::kPagePauseTicks - 1, pages);
    REQUIRE(visit.caption()->page == 0);
    visit.update(1, pages);
    REQUIRE(visit.caption()->page == 1);
    REQUIRE(visit.caption()->shown == 0);
    visit.update(16, pages);
    REQUIRE(visit.caption()->shown == 4);
    visit.update(BossVictory::kPagePauseTicks, pages);
    REQUIRE_FALSE(visit.caption().has_value());
    REQUIRE(visit.stage() == Stage::Defeat);
    voices = visit.update(BossVictory::kAfterDefeatTicks, {});
    REQUIRE(visit.stage() == Stage::Runes);
    REQUIRE(voices.size() == 1);
    REQUIRE(voices[0].sound == "S_RUNEVOX1G");
    REQUIRE(visit.caption()->message == "RUNE_PHRASE1");
    // Its one page read, a second's pause, and he sees them off: two seconds, the sparkle
    // over the last thirty-five ticks, then it is done.
    const std::array<std::string, 1> page{"ABCDEF"};
    visit.update(22, page);
    visit.update(BossVictory::kPagePauseTicks, page);
    REQUIRE_FALSE(visit.caption().has_value());
    visit.update(BossVictory::kAfterRunesTicks, {});
    REQUIRE(visit.stage() == Stage::Leaving);
    REQUIRE_FALSE(visit.sparkling());
    visit.update(BossVictory::kExitTicks - BossVictory::kExitSparkleTicks, {});
    REQUIRE(visit.sparkling());
    REQUIRE(visit.stage() == Stage::Gone);
    REQUIRE(visit.wizardShown());
    visit.update(BossVictory::kExitSparkleTicks, {});
    REQUIRE(visit.finished());
    REQUIRE_FALSE(visit.running());
    REQUIRE_FALSE(visit.wizardShown());
    visit.clear();
    REQUIRE(visit.stage() == Stage::None);
    // Temple Skorne waits ten seconds and has his own twelve-rune follow-up.
    BossVictory demon;
    demon.begin(42, 'E', 0, 0, true);
    for (s32 i = 0; i < BossVictory::kLongWaitTicks - 1; ++i) {
        demon.update(1, {});
    }
    REQUIRE(demon.stage() == Stage::Waiting);
    demon.update(1, {});
    REQUIRE(demon.stage() == Stage::Appearing);
    for (s32 i = 0; i < 64; ++i) {
        demon.update(1, {});
    }
    REQUIRE(demon.stage() == Stage::Defeat);
    demon.update(BossVictory::kPagePauseTicks * 2, std::array<std::string, 1>{""});
    demon.update(BossVictory::kAfterDefeatTicks, {});
    REQUIRE(demon.stage() == Stage::Runes);
    REQUIRE(demon.caption()->message == "SKORNE1_RUNE_NO");
    demon.update(BossVictory::kAfterRunesTicks, {});
    REQUIRE(demon.stage() == Stage::Leaving);
    demon.update(BossVictory::kExitTicks, {});
    REQUIRE(demon.stage() == Stage::Leaving);
}

TEST_CASE("boss speech uses retail character and punctuation budgets before the rune pause",
          "[game][screens][victory]") {
    // DoGoodWizard passes speech_frame >> 1 to CaptionTextSub. Its 1.75-unit
    // characters and extra 2 units at '.' total 9 for ABC.; the terminator needs
    // a positive remainder. Constants 0x80345a20/28 are 0.5/1.0 seconds, not ticks.
    for (const s32 ticks : {1, 2}) {
        CAPTURE(ticks);
        BossVictory visit;
        visit.begin(41, 'G', 1, 0, false);
        visit.update(300, {});
        visit.update(64, {});
        const std::array<std::string, 1> pages{"ABC."};
        for (s32 elapsed = 0; elapsed < 18; elapsed += ticks) {
            REQUIRE(visit.update(ticks, pages).empty());
        }
        REQUIRE(visit.caption()->shown == 4);
        // All glyphs visible is not end-of-page: punctuation still consumes time.
        visit.update(2, pages);
        REQUIRE(visit.caption()->page == 0);
        for (s32 elapsed = 0; elapsed < 60; elapsed += ticks) {
            REQUIRE(visit.update(ticks, pages).empty());
        }
        REQUIRE_FALSE(visit.caption());
        for (s32 elapsed = 0; elapsed < 30 - ticks; elapsed += ticks) {
            REQUIRE(visit.update(ticks, {}).empty());
            REQUIRE(visit.stage() == Stage::Defeat);
        }
        const auto voices = visit.update(ticks, {});
        REQUIRE(voices.size() == 1);
        REQUIRE(voices.front().sound == "S_RUNEVOX0G");
        REQUIRE(visit.stage() == Stage::Runes);
        // Even missing captions retain the one-second post-rune pause.
        for (s32 elapsed = 0; elapsed < 60 - ticks; elapsed += ticks) {
            REQUIRE(visit.update(ticks, {}).empty());
            REQUIRE(visit.stage() == Stage::Runes);
        }
        visit.update(ticks, {});
        REQUIRE(visit.stage() == Stage::Leaving);
        visit.update(600, {}, true);
        REQUIRE(visit.stage() == Stage::Leaving);
        REQUIRE_FALSE(visit.sparkling());
        visit.update(85, {}, false);
        REQUIRE(visit.sparkling());
    }
}

TEST_CASE("boss caption line feeds are free but tabs and punctuation delay the next word",
          "[game][screens][victory]") {
    BossVictory visit;
    visit.begin(41, 'G', 0, 0, false);
    visit.update(300, {});
    visit.update(64, {});
    const std::array<std::string, 1> pages{"A,\n\tB"};
    visit.update(2, pages);
    REQUIRE(visit.caption()->shown == 1);
    visit.update(2, pages);
    REQUIRE(visit.caption()->shown == 2);
    visit.update(6, pages);
    REQUIRE(visit.caption()->shown == 2); // comma budget still exhausted
    visit.update(2, pages);
    REQUIRE(visit.caption()->shown == 4); // free newline followed by a five-unit tab
    visit.update(8, pages);
    REQUIRE(visit.caption()->shown == 4);
    visit.update(2, pages);
    REQUIRE(visit.caption()->shown == 5);
}

TEST_CASE("Skorne uses one combined voice and the first twelve runes for his follow-up",
          "[skorne][victory]") {
    for (const u16 runes : {u16{0}, u16{0x1000}, u16{0x0ffe}, u16{0x0fff}, u16{0x1fff}}) {
        BossVictory visit;
        visit.begin(42, 'E', 0, runes, false);
        visit.update(BossVictory::kLongWaitTicks, {});
        const auto voices = visit.update(64, {});
        const bool complete = (runes & 0x0fffU) == 0x0fffU;
        REQUIRE(voices.size() == 1);
        REQUIRE(voices.front().sound == (complete ? "S_E2VOXB" : "S_E2VOXA"));
        REQUIRE(visit.update(BossVictory::kAfterDefeatTicks, {}).empty());
        REQUIRE(visit.stage() == Stage::Runes);
        REQUIRE(visit.caption()->message == (complete ? "SKORNE1_RUNE_YES" : "SKORNE1_RUNE_NO"));
        REQUIRE(visit.update(BossVictory::kAfterRunesTicks, {}).empty());
        REQUIRE(visit.stage() == Stage::Leaving);
    }
}

constexpr s32 kLowest = std::numeric_limits<s32>::min();

TEST_CASE("every boss code keeps its defeat speech and voice and its rune voice at each quality",
          "[game][screens][victory][boss-kinds]") {
    struct Row {
        s32 code;
        std::string_view message;
        std::string_view voice;
        std::array<std::string_view, 4> runes; ///< by quality
    };
    // By quality (none found, some, the one, both): the first four realms' guardians say the
    // same for some as for all and the last four have a line of their own for all; the temple's
    // Skorne has one line for every quality, and every code after his none.
    using Runes = std::array<std::string_view, 4>;
    constexpr Runes kSomeIsAll{"S_RUNEVOX0G", "S_RUNEVOX1G", "S_RUNEVOX1G", "S_RUNEVOX1G"};
    constexpr Runes kOwnAll{"S_RUNEVOX0G", "S_RUNEVOX1G", "S_RUNEVOX2G", "S_RUNEVOX2G"};
    constexpr Runes kTemple{"S_E2VOXB", "S_E2VOXB", "S_E2VOXB", "S_E2VOXB"};
    constexpr Runes kNone{"", "", "", ""};
    constexpr std::array<Row, 15> kRows{{
        {-1, "", "S_DEFEATVOXG", kSomeIsAll},
        {33, "", "S_DEFEATVOXG", kSomeIsAll},
        {34, "DRAGON_SPEECH", "S_DEFEATVOXG", kSomeIsAll},
        {35, "CHIMERA_SPEECH", "S_DEFEATVOXG", kSomeIsAll},
        {36, "DJINN_SPEECH", "S_DEFEATVOXG", kSomeIsAll},
        {37, "DRIDER_SPEECH", "S_DEFEATVOXG", kSomeIsAll},
        {38, "PBOSS_SPEECH", "S_DEFEATVOXG", kOwnAll},
        {39, "YETI_SPEECH", "S_DEFEATVOXG", kOwnAll},
        {40, "WRAITH_SPEECH", "S_DEFEATVOXG", kOwnAll},
        {41, "LICH_SPEECH", "S_DEFEATVOXG", kOwnAll},
        {42, "SKORNE1_SPEECH", "S_E2VOXA", kTemple},
        {43, "SKORNE2_SPEECH", "S_ENDVOX", kNone},
        {44, "GARM2_SPEECH", "S_GRMDESTVOX", kNone},
        {45, "", "S_DEFEATVOXG", kNone},
        {kLowest, "", "S_DEFEATVOXG", kSomeIsAll},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        CHECK(BossVictory::defeatMessageOf(row.code) == row.message);
        CHECK(BossVictory::defeatVoiceOf(row.code, 'G') == row.voice);
        for (s32 quality = 0; quality < 4; ++quality) {
            CAPTURE(quality);
            CHECK(BossVictory::runeVoiceOf(row.code, 'G', quality) ==
                  row.runes[static_cast<usize>(quality)]);
        }
    }
}

TEST_CASE("every boss code keeps its wizard's wait and what follows the defeat speech",
          "[game][screens][victory][boss-kinds]") {
    struct Row {
        s32 code;
        s32 wait;                 ///< ticks from the fall to the wizard's coming
        Stage followUp;           ///< once the defeat speech is over
        std::string_view caption; ///< then up, with no runestone found
        std::string_view voice;   ///< then heard
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {33, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {34, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {35, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {36, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {37, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {38, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {39, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {40, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {41, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
        {42, 600, Stage::Runes, "SKORNE1_RUNE_NO", ""},
        {43, 300, Stage::Leaving, "", ""},
        {44, 600, Stage::Leaving, "", ""},
        {45, 300, Stage::Leaving, "", ""},
        {kLowest, 300, Stage::Runes, "RUNE_PHRASE0", "S_RUNEVOX0G"},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        BossVictory visit;
        visit.begin(row.code, 'G', 0, 0, false);
        s32 waited = 0;
        while (visit.stage() == Stage::Waiting && waited < 1000) {
            visit.update(1, {});
            ++waited;
        }
        CHECK(waited == row.wait);
        visit.update(64, {});
        REQUIRE(visit.stage() == Stage::Defeat);
        const auto voices = visit.update(BossVictory::kAfterDefeatTicks, {});
        CHECK(visit.stage() == row.followUp);
        const std::string_view caption =
            visit.caption().has_value() ? std::string_view{visit.caption()->message} : "";
        CHECK(caption == row.caption);
        REQUIRE(voices.size() == (row.voice.empty() ? 0U : 1U));
        if (!voices.empty()) {
            CHECK(voices.front().sound == row.voice);
        }
    }
}
} // namespace
