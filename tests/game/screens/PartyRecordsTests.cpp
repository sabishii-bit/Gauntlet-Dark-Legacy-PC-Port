#include <array>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "game/players/ItemPickup.h"
#include "game/players/NameCheats.h"
#include "game/screens/PartyRecords.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::array<PlayerRuntime, 2> twoPlayers() {
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(2, CharacterSave{}, nullptr, Vec3{0.0f}, 0.0f);
    players[1].actor.spawn(3, CharacterSave{}, nullptr, Vec3{0.0f}, 0.0f);
    players[1].slot = 4;
    for (PlayerRuntime& runtime : players) {
        runtime.entrySave = runtime.actor.save();
    }
    return players;
}
TEST_CASE("lifetime counters separate kills from generators and count only live players",
          "[party-records][shop]") {
    auto players = twoPlayers();
    PartyRecords::award(players, 2, 0, true, nullptr);
    PartyRecords::award(players, 2, 50, false, nullptr);
    PartyRecords::destroyedGenerator(players, 2);
    PartyRecords::advanceTime(players, 1.5);
    const auto& stats = players[0].actor.save().progress().lifetime;
    CHECK(stats.enemiesKilled == 1);
    CHECK(stats.generatorsDestroyed == 1);
    CHECK(stats.playSeconds == 1.5);
    CHECK(players[0].levelKills == 1);
    CHECK(players[0].turbo.held() == 0);
    players[0].life = PlayerLife::Dying;
    players[1].departed = true;
    PartyRecords::advanceTime(players, 10);
    PartyRecords::destroyedGenerator(players, 2);
    CHECK(stats.playSeconds == 1.5);
    CHECK(stats.generatorsDestroyed == 1);
    CHECK(players[1].actor.save().progress().lifetime.playSeconds == 1.5);
}

TEST_CASE("experience won goes to its player by identity and feeds the meter on a kill",
          "[game][screens][party-records]") {
    auto players = twoPlayers();
    PartyRecords::award(players, 3, 100, true, nullptr);
    CHECK(players[0].actor.save().experience() == 0);
    CHECK(players[1].actor.save().experience() == 100);
    CHECK(players[1].levelKills == 1);
    CHECK(players[1].turbo.held() == Approx(TurboMeter::kPerExperience * 100.0f));
    // Won otherwise, it feeds nothing; a kill worth nothing still counts.
    PartyRecords::award(players, 3, 40, false, nullptr);
    CHECK(players[1].actor.save().experience() == 140);
    CHECK(players[1].levelKills == 1);
    PartyRecords::award(players, 2, 0, true, nullptr);
    CHECK(players[0].levelKills == 1);
    // The fallen win nothing.
    players[0].life = PlayerLife::Dying;
    PartyRecords::award(players, 2, 100, true, nullptr);
    CHECK(players[0].actor.save().experience() == 0);
    CHECK(players[0].levelKills == 1);
}

TEST_CASE("the party goes on with what it gathered, the fallen as they came, the departed not",
          "[game][screens][party-records]") {
    auto players = twoPlayers();
    players[0].actor.save().gold = 50;
    players[0].actor.save().helpSeen = {9};
    players[0].helpHeard = {12};
    players[1].actor.save().gold = 70;
    players[1].actor.save().helpSeen = {4};
    players[1].life = PlayerLife::InTower;
    std::vector<PartyMember> members = PartyRecords::members(players);
    REQUIRE(members.size() == 2);
    CHECK(members[0].player == 2);
    CHECK(members[0].save.gold == 50);
    CHECK(members[0].helpHeard == std::vector<s32>{12});
    CHECK_FALSE(members[0].fallen);
    CHECK(members[1].save.gold == 0); // as it came in
    CHECK(members[1].save.helpSeen == std::vector<s32>{4});
    CHECK(members[1].fallen);
    CHECK(members[1].slot == std::optional<usize>{4});
    const std::vector<LevelResults> results = PartyRecords::results(players);
    REQUIRE(results.size() == 1);
    CHECK(results[0].player == 2);
    CHECK(results[0].totals[0] == 50);

    // Giving the level up, everyone is as they came in but keeps what they were taught.
    const std::vector<PartyMember> left = PartyRecords::abandoned(players, members);
    REQUIRE(left.size() == 2);
    CHECK(left[0].save.gold == 0);
    CHECK(left[0].save.helpSeen == std::vector<s32>{9});
    CHECK_FALSE(left[1].fallen);

    players[1].departed = true;
    CHECK(PartyRecords::members(players).size() == 1);
}

TEST_CASE("combo experience applies the partner's own scaling without sharing kill rewards",
          "[game][screens][party-records]") {
    auto players = twoPlayers(); // controller identities 2/3, combo indices 0/1
    players[0].actor.save().progress().experience = levelExperience(10);
    players[1].actor.save().progress().experience = levelExperience(20);
    const s32 beforeFirst = players[0].actor.save().experience();
    const s32 beforePartner = players[1].actor.save().experience();
    ComboMove::link(players[0].combo, 0, players[1].combo, 1, ComboMove::kWarrior);
    LevelInfo level;
    level.tuning.playerLevel = 10;
    level.tuning.experience = 0.5f;

    PartyRecords::award(players, 2, 120, true, &level);
    CHECK(players[0].actor.save().experience() == beforeFirst + 60);
    // The partner receives 60 * (0.5 / (1 + 0.1 * (20 - 10))) = 15, not 60.
    CHECK(players[1].actor.save().experience() == beforePartner + 15);
    CHECK(players[0].levelKills == 1);
    CHECK(players[1].levelKills == 0);
    CHECK(players[1].actor.save().progress().lifetime.enemiesKilled == 0);
    CHECK(players[1].turbo.held() == 0);

    players[1].departed = true;
    PartyRecords::award(players, 2, 120, true, &level);
    CHECK(players[1].actor.save().experience() == beforePartner + 15);
}

TEST_CASE("departed party slots cannot collect rewards or appear in level results",
          "[game][screens][party-records]") {
    auto players = twoPlayers();
    players[0].departed = true;
    PartyRecords::award(players, 2, 100, true, nullptr);
    PartyRecords::destroyedGenerator(players, 2);
    CHECK(players[0].actor.save().experience() == 0);
    CHECK(players[0].levelKills == 0);
    CHECK(players[0].actor.save().progress().lifetime.enemiesKilled == 0);
    CHECK(players[0].actor.save().progress().lifetime.generatorsDestroyed == 0);
    const auto results = PartyRecords::results(players);
    REQUIRE(results.size() == 1);
    CHECK(results[0].player == 3);
}

TEST_CASE("hidden Sumner restores health without rolling back the level's progress",
          "[game][screens][party-records][multiplayer]") {
    const s32 level = GENERATE(30, 99);
    auto players = twoPlayers();
    CharacterSave& sumner = players[1].actor.save();
    sumner.name = "SUM224";
    REQUIRE(applyNameCheats(sumner));
    REQUIRE(sumner.character == 2);
    sumner.progress().experience = levelExperience(level);
    sumner.progress().health = 700;
    sumner.gold = 10;
    players[1].entrySave = sumner;
    sumner.gold = 70;
    sumner.progress().health = 0;
    sumner.progress().relics.addRune(7);
    sumner.helpSeen = {4};
    players[1].helpHeard = {12};
    players[1].life = PlayerLife::InTower;
    players[0].actor.save().gold = 50;

    SECTION("waiting after death") {}
    SECTION("aborting while still standing") {
        players[1].life = PlayerLife::Standing;
        sumner.progress().health = 100;
    }
    const auto party = PartyRecords::members(players);
    REQUIRE(party.size() == 2);
    CHECK(party[1].player == 3);
    CHECK(party[1].slot == std::optional<usize>{4});
    CHECK(party[1].save.gold == 70);
    CHECK(party[1].save.progress().relics.hasRune(7));
    CHECK(party[1].save.progress().health ==
          (players[1].life == PlayerLife::Standing ? 100 : mostHealth(level)));
    const auto abandoned = PartyRecords::abandoned(players, party);
    REQUIRE(abandoned.size() == 2);
    CHECK(abandoned[0].save.gold == 0); // Ordinary characters still restore their checkpoint.
    CHECK(abandoned[1].save.gold == 70);
    CHECK(abandoned[1].save.progress().relics.hasRune(7));
    CHECK(abandoned[1].save.progress().health == mostHealth(level));
    CHECK(abandoned[1].save.helpSeen == std::vector<s32>{4});
    CHECK(abandoned[1].helpHeard == std::vector<s32>{12});
    CHECK_FALSE(abandoned[1].fallen);
}

TEST_CASE("Sumner's name on a different class does not exempt it from checkpoint restoration",
          "[game][screens][party-records]") {
    auto players = twoPlayers();
    players[0].actor.save().name = "SUM224";
    players[0].actor.save().gold = 50;
    players[0].life = PlayerLife::InTower;
    const auto party = PartyRecords::members(players);
    REQUIRE(party.size() == 2);
    CHECK(party[0].save.gold == 0);
}

} // namespace
