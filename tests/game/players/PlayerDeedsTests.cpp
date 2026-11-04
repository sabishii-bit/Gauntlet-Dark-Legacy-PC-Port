#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/PlayerDeeds.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A question party motion or the animator asks of a deed, and the deeds whose comparisons
 * answered yes to it before the table did. */
struct DeedSet {
    std::string_view name;
    u32 trait;
    std::vector<PlayerDeed> deeds;
};

bool holds(const std::vector<PlayerDeed>& deeds, PlayerDeed deed) {
    return std::ranges::find(deeds, deed) != deeds.end();
}

TEST_CASE("each deed trait holds the deeds its list of comparisons named",
          "[game][players][player-deeds]") {
    using enum PlayerDeed;
    const std::vector<DeedSet> sets{
        {"the close attacks (actionPace, meleeRequest)",
         deed_trait::kClose,
         {AutoMelee, AutoMeleeLow, Melee, MeleeLow, MeleeSlow, MeleeSlowLow}},
        {"the low strikes (meleeRequest)",
         deed_trait::kLow,
         {MeleeLow, MeleeSlowLow, AutoMeleeLow}},
        {"the slow swings (meleeRequest)", deed_trait::kStrong, {MeleeSlow, MeleeSlowLow}},
        {"the unpressed swings (update, meleeRequest, step)",
         deed_trait::kAutomatic,
         {AutoMelee, AutoMeleeLow}},
        {"the attack button held (update)", deed_trait::kAttackButton, {Attack, Melee, MeleeLow}},
        {"the strong button held (update)",
         deed_trait::kStrongButton,
         {StrongAttack, MeleeSlow, MeleeSlowLow}},
        {"the item attacks (actionPace)",
         deed_trait::kItem,
         {SuperShot, Hammer, Breathe, FireLeft, FireRight}},
        {"the gauntlets (useItemAttack, animate)", deed_trait::kGauntlet, {FireLeft, FireRight}},
        {"the legend gestures (update)",
         deed_trait::kLegend,
         {HurlLegend, ThrowLegend, ShootLegend}},
        {"the potions that cut an ordinary attack (update)",
         deed_trait::kPotion,
         {UsePotion, ThrowPotion, ShieldPotion}},
        {"what begins over an ordinary attack (canBegin)",
         deed_trait::kCutsAttack,
         {TurboStrong, TurboFull, SuperShot, Hammer, Breathe, Combo}},
        {"what begins during a close attack (canBegin)",
         deed_trait::kCutsMelee,
         {TurboStrong, TurboFull, Shove, Combo}},
        {"what begins with the guard raised (canBegin)",
         deed_trait::kCutsGuard,
         {TurboStrong, TurboFull, Shove, Combo, SuperShot, Hammer, Breathe, HurlLegend,
          ShootLegend}},
        {"what holds the body still while asked (update)",
         deed_trait::kHoldsBody,
         {TurboStrong, TurboFull, Shove, StrongAttack, HurlLegend, ThrowLegend, ShootLegend}},
        {"the standing attacks that turn to the assisted target (step)",
         deed_trait::kAimed,
         {Attack, StrongAttack, Melee, MeleeLow, MeleeSlow, MeleeSlowLow, Hammer, Breathe, FireLeft,
          FireRight}},
        {"the blows that fell (update)", deed_trait::kFells, {FallBack, FallForward, Whirled}},
        {"the blows that strike (update)",
         deed_trait::kStrikes,
         {Flinch, Reel, Spike, Webbed, FallBack, FallForward, Whirled}},
    };
    u32 traits = 0;
    for (const DeedSet& set : sets) {
        CAPTURE(set.name);
        CHECK((traits & set.trait) == 0);
        traits |= set.trait;
        for (usize value = 0; value < kPlayerDeedCount; ++value) {
            const auto deed = static_cast<PlayerDeed>(value);
            CAPTURE(value);
            CHECK(deedHas(deed, set.trait) == holds(set.deeds, deed));
        }
    }
    // Every bit the table sets is one of these questions.
    for (usize value = 0; value < kPlayerDeedCount; ++value) {
        CAPTURE(value);
        const DeedTraits* row = deedTraitsOf(static_cast<PlayerDeed>(value));
        REQUIRE(row != nullptr);
        CHECK(row->deed == static_cast<PlayerDeed>(value));
        CHECK((row->traits & ~traits) == 0);
        CHECK_FALSE(deedHas(row->deed, 0));
    }
}

TEST_CASE("the deed traits combine into the swings party motion and the animator tell apart",
          "[game][players][player-deeds]") {
    using enum PlayerDeed;
    // A quick swing at something not low starts without turning (actionPace, attackMove); a
    // close swing of the attack button carries a held chain on (update).
    const std::vector<PlayerDeed> quickHigh{Melee, AutoMelee};
    const std::vector<PlayerDeed> pressedQuick{Melee, MeleeLow};
    for (usize value = 0; value < kPlayerDeedCount; ++value) {
        const auto deed = static_cast<PlayerDeed>(value);
        CAPTURE(value);
        CHECK((deedHas(deed, deed_trait::kClose) &&
               !deedHas(deed, deed_trait::kStrong | deed_trait::kLow)) == holds(quickHigh, deed));
        CHECK((deedHas(deed, deed_trait::kClose) && deedHas(deed, deed_trait::kAttackButton)) ==
              holds(pressedQuick, deed));
        // What may begin during a close attack may begin with the guard raised.
        CHECK((!deedHas(deed, deed_trait::kCutsMelee) || deedHas(deed, deed_trait::kCutsGuard)));
        // A blow that fells strikes.
        CHECK((!deedHas(deed, deed_trait::kFells) || deedHas(deed, deed_trait::kStrikes)));
    }
}

TEST_CASE("each deed ranks against a pending reaction as an impact combines them",
          "[game][players][player-deeds][reaction-rank]") {
    using enum PlayerDeed;
    struct Rank {
        PlayerDeed deed;
        u8 rank;
    };
    constexpr std::array<Rank, kPlayerDeedCount> kRanks{{
        {None, 0},         {Attack, 0},     {UsePotion, 0},    {ThrowPotion, 0},  {Die, 0},
        {Flinch, 3},       {Spike, 2},      {Reel, 1},         {TurboStrong, 0},  {TurboFull, 0},
        {Shove, 0},        {Defend, 0},     {StrongAttack, 0}, {ShieldPotion, 0}, {FallBack, 4},
        {FallForward, 4},  {HurlLegend, 0}, {ThrowLegend, 0},  {ShootLegend, 0},  {Grabbed, 0},
        {Thrown, 0},       {Webbed, 1},     {Melee, 0},        {MeleeLow, 0},     {MeleeSlow, 0},
        {MeleeSlowLow, 0}, {AutoMelee, 0},  {AutoMeleeLow, 0}, {SuperShot, 0},    {Hammer, 0},
        {Breathe, 0},      {FireLeft, 0},   {FireRight, 0},    {Pick, 0},         {Gag, 0},
        {DeathGrab, 0},    {Whirled, 5},    {Combo, 0},        {ComboHeld, 0},    {ComboThrown, 0},
    }};
    for (usize value = 0; value < kPlayerDeedCount; ++value) {
        CAPTURE(value);
        REQUIRE(kRanks[value].deed == static_cast<PlayerDeed>(value));
        CHECK(reactionRank(kRanks[value].deed) == kRanks[value].rank);
    }
}

TEST_CASE("a close swing's kind and height name its deed", "[game][players][player-deeds]") {
    using enum PlayerDeed;
    STATIC_CHECK(closeDeedOf(CloseSwing::Quick, CloseHeight::High) == Melee);
    STATIC_CHECK(closeDeedOf(CloseSwing::Quick, CloseHeight::Low) == MeleeLow);
    STATIC_CHECK(closeDeedOf(CloseSwing::Slow, CloseHeight::High) == MeleeSlow);
    STATIC_CHECK(closeDeedOf(CloseSwing::Slow, CloseHeight::Low) == MeleeSlowLow);
    STATIC_CHECK(closeDeedOf(CloseSwing::Automatic, CloseHeight::High) == AutoMelee);
    STATIC_CHECK(closeDeedOf(CloseSwing::Automatic, CloseHeight::Low) == AutoMeleeLow);
}

TEST_CASE("nothing past the last deed has a trait or a rank", "[game][players][player-deeds]") {
    STATIC_CHECK(kPlayerDeedCount == 40);
    for (usize value = kPlayerDeedCount; value <= 0xFF; ++value) {
        const auto deed = static_cast<PlayerDeed>(value);
        CAPTURE(value);
        CHECK(deedTraitsOf(deed) == nullptr);
        CHECK_FALSE(deedHas(deed, ~0U));
        CHECK(reactionRank(deed) == 0);
    }
}
} // namespace
