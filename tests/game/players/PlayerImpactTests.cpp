#include <filesystem>
#include <numbers>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/enemies/CritterData.h"
#include "game/players/PlayerImpact.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("damage flags select grounded reactions rather than damage amount alone",
          "[game][players][player-impact]") {
    PlayerImpact impact;
    REQUIRE(impact.reaction(100, 0, false) == PlayerDeed::None);
    impact.flags = PlayerImpact::kKnockBack;
    REQUIRE(impact.reaction(10, 0, false) == PlayerDeed::Flinch);
    REQUIRE(impact.reaction(2, 0, false) == PlayerDeed::None);
    REQUIRE(impact.reaction(10, 0, true) == PlayerDeed::None);
    impact.flags = PlayerImpact::kStun;
    REQUIRE(impact.reaction(1, 0, false) == PlayerDeed::Reel);
    REQUIRE(impact.reaction(0, 0, false) == PlayerDeed::None);
    impact.flags = PlayerImpact::kSpike;
    REQUIRE(impact.reaction(1, 0, false) == PlayerDeed::None);
    REQUIRE(impact.reaction(2, 0, false) == PlayerDeed::Spike);
    impact.flags |= PlayerImpact::kStun | PlayerImpact::kKnockBack;
    REQUIRE(impact.reaction(10, 0, false) == PlayerDeed::Flinch);
    REQUIRE(impact.reaction(2, 0, false) == PlayerDeed::Spike);
}

TEST_CASE("heavy blows fall with their direction and guards downgrade them to knockback",
          "[game][players][player-impact]") {
    for (const u32 flag :
         {PlayerImpact::kKnockDown, PlayerImpact::kBlownAway, PlayerImpact::kKnockOver}) {
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
    const PlayerImpact impact{PlayerImpact::kWhirlwind | PlayerImpact::kKnockDown, {0, 0, -1}};
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
        if ((attack.flags & PlayerImpact::kWhirlwind) != 0) {
            ++winds;
            const PlayerImpact impact{attack.flags, Vec3{0, 0, 1}};
            REQUIRE(impact.reaction(attack.damage > 2.0f ? attack.damage : 10.0f, 0, false) ==
                    PlayerDeed::Whirled);
        }
    }
    REQUIRE(winds >= 2); // DAMG records 13 and 14
}

TEST_CASE("a lesser contact cannot erase a queued knockdown", "[game][players][player-impact]") {
    const PlayerImpact web{PlayerImpact::kSticky, Vec3{0}};
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
} // namespace
