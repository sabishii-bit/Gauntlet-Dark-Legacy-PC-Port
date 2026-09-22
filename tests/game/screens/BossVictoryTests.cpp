#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/screens/BossVictory.h"

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
    for (int i = 0; i < BossVictory::kWaitTicks - 1; ++i) {
        REQUIRE(visit.update(1, {}).empty());
    }
    REQUIRE(visit.stage() == Stage::Waiting);
    visit.update(1, {});
    REQUIRE(visit.stage() == Stage::Appearing);
    REQUIRE(visit.wizardShown());
    REQUIRE(visit.wizardAlpha() == 0.0f);
    std::vector<VictoryVoice> voices;
    int fading = 0;
    while (visit.stage() == Stage::Appearing && fading < 200) {
        auto spoken = visit.update(1, {});
        voices.insert(voices.end(), spoken.begin(), spoken.end());
        ++fading;
    }
    REQUIRE(fading == 64);
    REQUIRE(visit.wizardAlpha() == 1.0f);
    // Then the defeat line, voiced from the level's bank, typed a character every two ticks.
    REQUIRE(visit.stage() == Stage::Defeat);
    REQUIRE(voices.size() == 1);
    REQUIRE(voices[0].sound == "S_DEFEATVOXG");
    REQUIRE(visit.caption().has_value());
    REQUIRE(visit.caption()->message == "LICH_SPEECH");
    REQUIRE(visit.caption()->page == 0);
    REQUIRE(visit.caption()->shown == 0);
    const std::array<usize, 2> pages{10, 4};
    visit.update(2, pages);
    REQUIRE(visit.caption()->shown == 1);
    visit.update(18, pages);
    REQUIRE(visit.caption()->shown == 10);
    // A second's pause, and the next page; past the last, half a second, then the runes.
    visit.update(BossVictory::kPagePauseTicks - 1, pages);
    REQUIRE(visit.caption()->page == 0);
    visit.update(1, pages);
    REQUIRE(visit.caption()->page == 1);
    REQUIRE(visit.caption()->shown == 0);
    visit.update(8, pages);
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
    const std::array<usize, 1> page{6};
    visit.update(12, page);
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
    // The demon and the garm keep the party ten seconds, and have no rune line.
    BossVictory demon;
    demon.begin(42, 'E', 0, 0, true);
    for (int i = 0; i < BossVictory::kLongWaitTicks - 1; ++i) {
        demon.update(1, {});
    }
    REQUIRE(demon.stage() == Stage::Waiting);
    demon.update(1, {});
    REQUIRE(demon.stage() == Stage::Appearing);
    for (int i = 0; i < 64; ++i) {
        demon.update(1, {});
    }
    REQUIRE(demon.stage() == Stage::Defeat);
    demon.update(BossVictory::kPagePauseTicks * 2, std::array<usize, 1>{0});
    demon.update(BossVictory::kAfterDefeatTicks, {});
    REQUIRE(demon.stage() == Stage::Leaving); // no rune line; the gold left keeps them
    demon.update(BossVictory::kExitTicks, {});
    REQUIRE(demon.stage() == Stage::Leaving);
}

} // namespace
