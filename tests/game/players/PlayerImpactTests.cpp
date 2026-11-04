#include <array>
#include <filesystem>
#include <numbers>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/combat/DamageTypes.h"
#include "game/critters/CritterData.h"
#include "game/players/PlayerImpact.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the heavy knocks are the DMG_ bits that put a player down",
          "[game][players][player-impact][flag-masks]") {
    STATIC_REQUIRE(PlayerImpact::kHeavy == 0x10160U);
}

TEST_CASE("damage flags select grounded reactions rather than damage amount alone",
          "[game][players][player-impact]") {
    PlayerImpact impact;
    REQUIRE(impact.reaction(100, 0, false) == PlayerDeed::None);
    impact.flags = damage::kKnockBack;
    REQUIRE(impact.reaction(10, 0, false) == PlayerDeed::Flinch);
    REQUIRE(impact.reaction(2, 0, false) == PlayerDeed::None);
    REQUIRE(impact.reaction(10, 0, true) == PlayerDeed::None);
    impact.flags = damage::kStun;
    REQUIRE(impact.reaction(1, 0, false) == PlayerDeed::Reel);
    REQUIRE(impact.reaction(0, 0, false) == PlayerDeed::Reel);
    REQUIRE(impact.reaction(-1, 0, false) == PlayerDeed::None);
    impact.flags = damage::kSpike;
    REQUIRE(impact.reaction(1, 0, false) == PlayerDeed::None);
    REQUIRE(impact.reaction(2, 0, false) == PlayerDeed::Spike);
    impact.flags |= damage::kStun | damage::kKnockBack;
    REQUIRE(impact.reaction(10, 0, false) == PlayerDeed::Flinch);
    REQUIRE(impact.reaction(2, 0, false) == PlayerDeed::Spike);
}

TEST_CASE("heavy blows fall with their direction and guards downgrade them to knockback",
          "[game][players][player-impact]") {
    for (const u32 flag : {damage::kKnockDown, damage::kBlownAway, damage::kKnockOver}) {
        CAPTURE(flag);
        PlayerImpact impact{flag, {0, 0, -1}};
        REQUIRE(impact.reaction(10, 0, false) == PlayerDeed::FallBack);
        REQUIRE(impact.reaction(10, std::numbers::pi_v<f32>, false) == PlayerDeed::FallForward);
        REQUIRE(impact.reaction(10, 0, true) == PlayerDeed::Flinch);
        REQUIRE(impact.reaction(2, 0, false) == PlayerDeed::None);
        REQUIRE(impact.reaction(2, 0, true) == PlayerDeed::None);
        impact.direction = {0, 0, 1};
        REQUIRE(impact.reaction(10, 0, false) == PlayerDeed::FallForward);
        REQUIRE(impact.reaction(10, 2 * std::numbers::pi_v<f32>, false) == PlayerDeed::FallForward);
    }
}

TEST_CASE("a whirlwind sweeps the player up before any other knock",
          "[game][players][player-impact]") {
    // PlayerKnockback tests 0x10000 first: with a knock down too, it is still the whirlwind.
    const PlayerImpact impact{damage::kWhirlwind | damage::kKnockDown, {0, 0, -1}};
    REQUIRE(impact.reaction(10, 0, false) == PlayerDeed::Whirled);
    REQUIRE(impact.reaction(10, std::numbers::pi_v<f32>, false) == PlayerDeed::Whirled);
    REQUIRE(impact.reaction(10, 0, true) == PlayerDeed::Flinch); // a guard takes it down
    REQUIRE(impact.reaction(2, 0, false) == PlayerDeed::None);
    REQUIRE(PlayerImpact::combine(PlayerDeed::FallBack, PlayerDeed::Whirled) ==
            PlayerDeed::Whirled);
    REQUIRE(PlayerImpact::combine(PlayerDeed::Whirled, PlayerDeed::FallForward) ==
            PlayerDeed::Whirled);
}

TEST_CASE("the genie's wind is the whirlwind", "[game][players][player-impact][assets]") {
    const std::filesystem::path file = test::assetOrSkip("CRITTER/DJINN.WAD");
    CritterData djinn;
    REQUIRE(djinn.load(file));
    s32 winds = 0;
    for (const AttackDefinition& attack : djinn.damages()) {
        if ((attack.flags & damage::kWhirlwind) != 0) {
            ++winds;
            const PlayerImpact impact{attack.flags, Vec3{0, 0, 1}};
            REQUIRE(impact.reaction(attack.damage > 2.0f ? attack.damage : 10.0f, 0, false) ==
                    PlayerDeed::Whirled);
        }
    }
    REQUIRE(winds >= 2); // DAMG records 13 and 14
}

TEST_CASE("a lesser contact cannot erase a queued knockdown", "[game][players][player-impact]") {
    const PlayerImpact web{damage::kSticky, Vec3{0}};
    REQUIRE(web.reaction(1, 0, false) == PlayerDeed::Webbed);
    REQUIRE(web.reaction(1, 0, true) == PlayerDeed::Webbed);
    REQUIRE(PlayerImpact::combine(PlayerDeed::FallBack, PlayerDeed::Webbed) ==
            PlayerDeed::FallBack);
    REQUIRE(PlayerImpact::combine(PlayerDeed::None, PlayerDeed::Reel) == PlayerDeed::Reel);
    REQUIRE(PlayerImpact::combine(PlayerDeed::Reel, PlayerDeed::Spike) == PlayerDeed::Spike);
    REQUIRE(PlayerImpact::combine(PlayerDeed::Reel, PlayerDeed::Flinch) == PlayerDeed::Flinch);
    REQUIRE(PlayerImpact::combine(PlayerDeed::Flinch, PlayerDeed::FallBack) ==
            PlayerDeed::FallBack);
    REQUIRE(PlayerImpact::combine(PlayerDeed::FallBack, PlayerDeed::Spike) == PlayerDeed::FallBack);
    REQUIRE(PlayerImpact::combine(PlayerDeed::FallForward, PlayerDeed::None) ==
            PlayerDeed::FallForward);
}

TEST_CASE("a pending reaction gives way only to a deed of higher rank, for every pair",
          "[game][players][player-impact][reaction-rank]") {
    // combine's rank of each deed: the higher wins, and a tie keeps the one pending.
    struct Rank {
        PlayerDeed deed;
        s32 rank;
    };
    constexpr usize kDeedCount = static_cast<usize>(PlayerDeed::ComboThrown) + 1;
    constexpr std::array<Rank, kDeedCount> kRanks{{
        {PlayerDeed::None, 0},         {PlayerDeed::Attack, 0},       {PlayerDeed::UsePotion, 0},
        {PlayerDeed::ThrowPotion, 0},  {PlayerDeed::Die, 0},          {PlayerDeed::Flinch, 3},
        {PlayerDeed::Spike, 2},        {PlayerDeed::Reel, 1},         {PlayerDeed::TurboStrong, 0},
        {PlayerDeed::TurboFull, 0},    {PlayerDeed::Shove, 0},        {PlayerDeed::Defend, 0},
        {PlayerDeed::StrongAttack, 0}, {PlayerDeed::ShieldPotion, 0}, {PlayerDeed::FallBack, 4},
        {PlayerDeed::FallForward, 4},  {PlayerDeed::HurlLegend, 0},   {PlayerDeed::ThrowLegend, 0},
        {PlayerDeed::ShootLegend, 0},  {PlayerDeed::Grabbed, 0},      {PlayerDeed::Thrown, 0},
        {PlayerDeed::Webbed, 1},       {PlayerDeed::Melee, 0},        {PlayerDeed::MeleeLow, 0},
        {PlayerDeed::MeleeSlow, 0},    {PlayerDeed::MeleeSlowLow, 0}, {PlayerDeed::AutoMelee, 0},
        {PlayerDeed::AutoMeleeLow, 0}, {PlayerDeed::SuperShot, 0},    {PlayerDeed::Hammer, 0},
        {PlayerDeed::Breathe, 0},      {PlayerDeed::FireLeft, 0},     {PlayerDeed::FireRight, 0},
        {PlayerDeed::Pick, 0},         {PlayerDeed::Gag, 0},          {PlayerDeed::DeathGrab, 0},
        {PlayerDeed::Whirled, 5},      {PlayerDeed::Combo, 0},        {PlayerDeed::ComboHeld, 0},
        {PlayerDeed::ComboThrown, 0},
    }};
    for (usize pending = 0; pending < kDeedCount; ++pending) {
        REQUIRE(kRanks[pending].deed == static_cast<PlayerDeed>(pending));
        for (usize incoming = 0; incoming < kDeedCount; ++incoming) {
            CAPTURE(pending, incoming);
            const auto& held = kRanks[pending];
            const auto& landed = kRanks[incoming];
            CHECK(PlayerImpact::combine(held.deed, landed.deed) ==
                  (landed.rank > held.rank ? landed.deed : held.deed));
        }
    }
}
} // namespace
