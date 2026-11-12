#include <array>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/world/ParticleSystem.h"

#include "TestSupport.h"
#include "game/bosses/LegendItems.h"
#include "game/players/PlayerAnimator.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("each boss has the legend item of its realm and its own weakness", "[game][enemies]") {
    // The ice axe (the mountain's, realm two) freezes the dragon for twenty seconds.
    const LegendWeakness* dragon = legendWeaknessOf(34);
    REQUIRE(dragon != nullptr);
    REQUIRE(dragon->realm == 2);
    REQUIRE(dragon->healthShare == Approx(0.1f));
    REQUIRE(dragon->frozenTicks == 1200);
    REQUIRE(legendRealmOf(34) == 2);
    // The lamp leaves the genie in the dark; the javelin blinds the plague fiend longer.
    REQUIRE(legendWeaknessOf(36)->blindTicks == 1800);
    REQUIRE(legendWeaknessOf(38)->realm == 11);
    REQUIRE(legendWeaknessOf(38)->blindTicks == 18000);
    // The bellows poison the spider for good, and shrink it; the parchment curbs the yeti
    // for a while; the lantern costs the wraith a flat sum; the book a quarter of the lich.
    const LegendWeakness* spider = legendWeaknessOf(37);
    REQUIRE(spider->curbs());
    REQUIRE(spider->recoverySeconds == 0.0f);
    REQUIRE(spider->scale == Approx(0.8f));
    REQUIRE(legendWeaknessOf(39)->recoverySeconds == Approx(29.0f));
    REQUIRE(legendWeaknessOf(38)->recoverySeconds == Approx(29.0f));
    REQUIRE(legendWeaknessOf(40)->damage == 500.0f);
    REQUIRE_FALSE(legendWeaknessOf(40)->healthShare > 0.0f);
    REQUIRE(legendWeaknessOf(41)->realm == 7);
    REQUIRE(legendWeaknessOf(41)->healthShare == Approx(0.25f));
    REQUIRE(legendWeaknessOf(35)->beheads);
    // The underworld's and the battlefield's bosses have none.
    REQUIRE(legendWeaknessOf(43) == nullptr);
    REQUIRE(legendWeaknessOf(44) == nullptr);
    REQUIRE(legendRealmOf(44) == 0);
    REQUIRE(legendWeaknessOf(-1) == nullptr);
}

TEST_CASE("the rite waits for the boss to rise, brandishes and throws, has it roar, and "
          "runs the weakness out",
          "[game][enemies]") {
    LegendRite rite;
    REQUIRE(rite.stage() == LegendRite::Stage::None);
    REQUIRE_FALSE(rite.running());
    REQUIRE(rite.update(2, true, false).empty());
    rite.begin(1, *legendWeaknessOf(39)); // the yeti: a curb that wears off
    REQUIRE(rite.stage() == LegendRite::Stage::Carried);
    REQUIRE(rite.player() == 1);
    REQUIRE(rite.holdsBoss());
    // Nothing while the boss is still rising.
    for (s32 i = 0; i < 100; ++i) {
        REQUIRE(rite.update(2, false, false).empty());
    }
    REQUIRE(rite.stage() == LegendRite::Stage::Carried);
    REQUIRE_FALSE(rite.darkens());
    // Risen: the item goes up at once, and is thrown a second on; the level goes dark
    // from here until the roar is over.
    auto cues = rite.update(2, true, false);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0] == LegendCue::Brandished);
    REQUIRE(rite.stage() == LegendRite::Stage::Woken);
    REQUIRE(rite.darkens());
    REQUIRE_FALSE(rite.thrown());
    s32 ticks = 2;
    while (!rite.thrown() && ticks < 600) {
        cues = rite.update(2, true, false);
        ticks += 2;
    }
    REQUIRE(rite.thrown());
    REQUIRE(ticks == LegendRite::kBrandishTicks);
    REQUIRE(cues.back() == LegendCue::Thrown);
    REQUIRE(rite.stage() == LegendRite::Stage::Struck);
    REQUIRE_FALSE(rite.holdsBoss());
    // The yeti roars three seconds after rising; the lich would in one.
    REQUIRE_FALSE(rite.wantsRoar());
    while (!rite.wantsRoar() && ticks < 600) {
        rite.update(2, true, false);
        ticks += 2;
    }
    REQUIRE(ticks == LegendRite::kLongRoarWait);
    REQUIRE(rite.darkens());
    // Its roar done, the curb runs twenty-nine seconds and wears off, in the light again.
    cues = rite.update(2, true, true);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0] == LegendCue::Roared);
    REQUIRE_FALSE(rite.wantsRoar());
    REQUIRE_FALSE(rite.darkens());
    REQUIRE(rite.running());
    s32 worn = 0;
    bool wornOff = false;
    while (rite.running() && worn < 60 * 40) {
        for (const LegendCue cue : rite.update(2, true, true)) {
            wornOff = wornOff || cue == LegendCue::WornOff;
        }
        worn += 2;
    }
    REQUIRE(wornOff);
    REQUIRE(rite.stage() == LegendRite::Stage::Over);
    REQUIRE(worn == Approx(29 * 60).margin(2));
    // A weakness for good ends the rite at the roar, with nothing to wear off.
    LegendRite forGood;
    forGood.begin(0, *legendWeaknessOf(37));
    forGood.update(2, true, false);
    for (s32 i = 0; i < 200; ++i) {
        forGood.update(2, true, false);
    }
    cues = forGood.update(2, true, true);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0] == LegendCue::Roared);
    REQUIRE(forGood.stage() == LegendRite::Stage::Over);
    forGood.clear();
    REQUIRE(forGood.stage() == LegendRite::Stage::None);
}

TEST_CASE("only the bearer carries the item into the fight, from the rite's start until the throw",
          "[game][enemies][legend][combo]") {
    LegendRite rite;
    CHECK_FALSE(rite.carriedBy(-1)); // no rite: nobody, not even the bearer of none
    CHECK_FALSE(rite.carriedBy(2));
    rite.begin(2, *legendWeaknessOf(41)); // the lich's book
    CHECK(rite.carriedBy(2));
    CHECK_FALSE(rite.carriedBy(0));
    // Still the bearer's while the boss rises and as the item is held up.
    rite.update(2, false, false);
    CHECK(rite.carriedBy(2));
    rite.update(2, true, false);
    REQUIRE(rite.stage() == LegendRite::Stage::Woken);
    CHECK(rite.carriedBy(2));
    for (s32 ticks = 0; !rite.thrown() && ticks < LegendRite::kBrandishTicks; ticks += 2) {
        rite.update(2, true, false);
    }
    REQUIRE(rite.thrown());
    CHECK_FALSE(rite.carriedBy(2));
    rite.clear();
    CHECK_FALSE(rite.carriedBy(2));
}

TEST_CASE("the rite holds the party turned to the boss from its rising until its roar is due, "
          "and restrains it from the level's start until the boss has roared",
          "[game][enemies][legend][rite-hold]") {
    struct Wait {
        s32 kind;
        s32 ticks; ///< from the rising to the roar falling due
    };
    // The Chimera's second (also the lich's and the temple's), the yeti's three.
    for (const Wait wait :
         {Wait{35, LegendRite::kShortRoarWait}, Wait{39, LegendRite::kLongRoarWait},
          Wait{37, LegendRite::kLongRoarWait}}) {
        CAPTURE(wait.kind);
        LegendRite rite;
        CHECK_FALSE(rite.holdsParty());
        CHECK_FALSE(rite.restrainsParty());
        // Carried in (state 1): restrained, not yet held, however long the boss takes.
        rite.begin(0, *legendWeaknessOf(wait.kind));
        for (s32 i = 0; i < 100; ++i) {
            rite.update(2, false, false);
            REQUIRE(rite.restrainsParty());
            REQUIRE_FALSE(rite.holdsParty());
        }
        // Risen (state 2): held, through the brandishing and the throw.
        rite.update(2, true, false);
        s32 ticks = 2;
        while (rite.holdsParty() && ticks < 600) {
            REQUIRE(rite.restrainsParty());
            rite.update(2, true, false);
            ticks += 2;
        }
        CHECK(ticks == wait.ticks);
        CHECK(rite.thrown());
        // The roar due (state 3): no longer held, still restrained until it is heard.
        CHECK(rite.wantsRoar());
        CHECK(rite.restrainsParty());
        rite.update(2, true, false);
        CHECK(rite.restrainsParty());
        rite.update(2, true, true);
        CHECK_FALSE(rite.restrainsParty());
        CHECK_FALSE(rite.holdsParty());
    }
    SECTION("the axe ends the dragon's hold as it lands, before its wait is out") {
        LegendRite rite;
        rite.begin(0, *legendWeaknessOf(34));
        rite.update(2, true, false);
        rite.update(LegendRite::kBrandishTicks, true, false);
        REQUIRE(rite.thrown());
        REQUIRE(rite.holdsParty());
        REQUIRE(rite.finishOnImpact());
        CHECK_FALSE(rite.holdsParty());
        CHECK_FALSE(rite.restrainsParty());
    }
    SECTION("the lamp landing after the genie's roar ends nothing more") {
        LegendRite rite;
        rite.begin(0, *legendWeaknessOf(36));
        rite.update(2, true, false);
        rite.update(LegendRite::kLongRoarWait, true, false);
        CHECK_FALSE(rite.holdsParty());
        CHECK(rite.restrainsParty());
        rite.update(2, true, true);
        CHECK_FALSE(rite.restrainsParty());
        REQUIRE(rite.finishOnImpact());
        CHECK_FALSE(rite.holdsParty());
        CHECK_FALSE(rite.restrainsParty());
    }
    SECTION("cleared, the rite holds and restrains nobody") {
        LegendRite rite;
        rite.begin(0, *legendWeaknessOf(41));
        rite.update(2, true, false);
        REQUIRE(rite.holdsParty());
        rite.clear();
        CHECK_FALSE(rite.holdsParty());
        CHECK_FALSE(rite.restrainsParty());
    }
    SECTION("concluded as its boss's death plays, the rite holds and restrains nobody again") {
        // world_update's state 99 (0x80056d04-0x80056d14), from rite state 2 and from state 1.
        LegendRite rite;
        rite.begin(0, *legendWeaknessOf(41));
        rite.update(2, true, false);
        REQUIRE(rite.holdsParty());
        rite.conclude();
        CHECK(rite.stage() == LegendRite::Stage::Over);
        CHECK_FALSE(rite.holdsParty());
        CHECK_FALSE(rite.restrainsParty());
        // Over, its roar's wait running out and the boss roaring hold and restrain nobody.
        rite.update(LegendRite::kLongRoarWait, true, true);
        CHECK_FALSE(rite.holdsParty());
        CHECK_FALSE(rite.restrainsParty());
        LegendRite carried;
        carried.begin(0, *legendWeaknessOf(41));
        REQUIRE(carried.restrainsParty());
        carried.conclude();
        CHECK_FALSE(carried.restrainsParty());
        carried.update(2, true, false);
        CHECK_FALSE(carried.holdsParty());
        CHECK_FALSE(carried.restrainsParty());
    }
}

TEST_CASE("the dragon's item presentation ends on impact without waiting for it to thaw or roar",
          "[game][enemies][legend]") {
    LegendRite rite;
    REQUIRE_FALSE(rite.finishOnImpact());
    rite.begin(0, *legendWeaknessOf(34));
    REQUIRE_FALSE(rite.finishOnImpact());
    rite.update(1, true, false);
    REQUIRE(rite.darkens());
    REQUIRE_FALSE(rite.finishOnImpact());
    rite.update(LegendRite::kBrandishTicks, true, false);
    REQUIRE(rite.thrown());
    REQUIRE(rite.darkens());
    SECTION("the axe lands before a roar can finish") {
        REQUIRE(rite.finishOnImpact());
    }
    SECTION("a completed roar cannot discard the pending impact") {
        rite.update(LegendRite::kLongRoarWait, true, true);
        REQUIRE(rite.running());
        REQUIRE(rite.darkens());
        REQUIRE(rite.finishOnImpact());
    }
    REQUIRE_FALSE(rite.darkens());
    REQUIRE_FALSE(rite.running());
    REQUIRE_FALSE(rite.wantsRoar());
    REQUIRE_FALSE(rite.finishOnImpact()); // no second application of damage or freeze
    REQUIRE(rite.update(1200, true, false).empty());
    REQUIRE_FALSE(rite.darkens());

    rite.begin(0, *legendWeaknessOf(39));
    rite.update(LegendRite::kBrandishTicks, true, false);
    REQUIRE_FALSE(rite.finishOnImpact()); // other bosses still use their own sequence
    REQUIRE(rite.darkens());
}

TEST_CASE("the genie lamp awaits impact even after the boss has finished roaring",
          "[game][enemies][legend][genie]") {
    LegendRite rite;
    rite.begin(0, *legendWeaknessOf(36));
    REQUIRE_FALSE(rite.finishOnImpact());
    rite.update(LegendRite::kBrandishTicks, true, false);
    rite.update(LegendRite::kLongRoarWait, true, true);
    REQUIRE(rite.running());
    REQUIRE(rite.finishOnImpact());
    REQUIRE_FALSE(rite.darkens());
    REQUIRE_FALSE(rite.finishOnImpact());
}

TEST_CASE("the rite is shown by the boss's kind: the hold, the gesture, the flight and the "
          "burst",
          "[game][enemies]") {
    // The scimitar, the axe, the lamp, the bellows, the javelin and the parchment are held
    // in the hand and the rest over the head.
    REQUIRE(LegendShow::heldInHand(34));
    REQUIRE(LegendShow::heldInHand(39));
    REQUIRE_FALSE(LegendShow::heldInHand(40));
    REQUIRE_FALSE(LegendShow::heldInHand(41));
    // The genie's and the spider's items go with the special shot, the dragon's, chimera's,
    // plague fiend's and yeti's with the strong throw, the rest as a potion is used.
    REQUIRE(LegendShow::gestureOf(36) == PlayerDeed::ShootLegend);
    REQUIRE(LegendShow::gestureOf(37) == PlayerDeed::ShootLegend);
    REQUIRE(LegendShow::gestureOf(34) == PlayerDeed::ThrowLegend);
    REQUIRE(LegendShow::gestureOf(39) == PlayerDeed::ThrowLegend);
    REQUIRE(LegendShow::gestureOf(41) == PlayerDeed::HurlLegend);
    REQUIRE(LegendShow::gestureOf(42) == PlayerDeed::HurlLegend);
    // Four fly at the boss; the bellows ride ahead of the bearer; the rest are set on it.
    REQUIRE(LegendShow::flightOf(35) == LegendShow::Flight::Flies);
    REQUIRE(LegendShow::flightOf(37) == LegendShow::Flight::WithBearer);
    REQUIRE(LegendShow::flightOf(41) == LegendShow::Flight::AtBoss);
    REQUIRE(LegendShow::flightOf(40) == LegendShow::Flight::AtBoss);
    // The book burns the lich for five seconds, the bellows the spider for three, the rest
    // for half a minute; the parchment's fire is its burst and then the second.
    REQUIRE(LegendShow::burstSecondsOf(41) == Approx(5.0f));
    REQUIRE(LegendShow::burstSecondsOf(37) == Approx(3.0f));
    REQUIRE(LegendShow::burstSecondsOf(40) == Approx(30.0f));
    REQUIRE(LegendShow::restingTreeOf(41) == "LEGENDPRJ");
    REQUIRE(LegendShow::burstTreeOf(41) == "LEGENDFX");
    REQUIRE(LegendShow::restingTreeOf(39) == "LEGENDFX");
    REQUIRE(LegendShow::burstTreeOf(39) == "LEGENDFX2");
    REQUIRE(LegendShow::bossOffsetOf(41) == Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(LegendShow::bossOffsetOf(39) == Vec3{-7, -5, 85});
    REQUIRE(LegendShow::bossOffsetOf(40) == Vec3{0, -5, 10});
    REQUIRE(LegendShow::bossOffsetOf(42) == Vec3{0, 17, 3});
    // The sounds are the realm's, by letter, with the dream's throw as the bank misspells it.
    REQUIRE(LegendShow::soundNamesOf(LegendShow::Sound::PickedUp, 'G').front() == "S_LEGWPUP");
    REQUIRE(LegendShow::soundNamesOf(LegendShow::Sound::Thrown, 'G').front() == "S_GLEGWTHROW");
    REQUIRE(LegendShow::soundNamesOf(LegendShow::Sound::Thrown, 'J').back() == "S_JEGWTHROW");
    REQUIRE(LegendShow::soundNamesOf(LegendShow::Sound::Flying, 'A') ==
            std::vector<std::string>{"S_ALEGWFLY", "S_ALEGWALL"});
    REQUIRE(LegendShow::soundNamesOf(LegendShow::Sound::Landed, 'G') ==
            std::vector<std::string>{"S_GLEGWHIT", "S_GLEGWALSTP"});
    REQUIRE(LegendShow::soundNamesOf(LegendShow::Sound::WornOff, 'K') ==
            std::vector<std::string>{"S_KLEGWPDN"});
}

TEST_CASE("legend charge colours and projectile particles follow the item's retail settings",
          "[game][legend]") {
    REQUIRE(LegendShow::chargeTree(0) == "COMBO_YEL");
    REQUIRE(LegendShow::chargeTree(1) == "COMBO_BLU");
    REQUIRE(LegendShow::chargeTree(2) == "COMBO_RED");
    REQUIRE(LegendShow::chargeTree(3) == "COMBO_GRN");
    REQUIRE(LegendShow::chargeTint(1) == Color::rgba(0, 0, 255));
    const ParticleDescriptor trail = LegendShow::trailOf(34);
    REQUIRE(trail.texture == "PARTICLE1_A");
    REQUIRE(trail.emitFrames == 90);
    REQUIRE(trail.fadeFrames == 1);
    REQUIRE(trail.particleLife == 30);
    REQUIRE(trail.particleFade == 30);
    REQUIRE(trail.speed == Approx(1.0f / 30.0f));
    REQUIRE(trail.width.at(10.0f, 30.0f, 30.0f) == Approx(2.0f));
    REQUIRE(trail.alpha.at(45.0f, 30.0f, 30.0f) == Approx(127.5f));
    REQUIRE_FALSE(trail.depthWrite);
    REQUIRE_FALSE(trail.dynamic);
    REQUIRE(LegendShow::trailOf(35).texture == "CHIMKEY_PART");
    REQUIRE(LegendShow::trailOf(38).texture == "PARTICLE1_A");
    REQUIRE(LegendShow::trailOf(41).texture.empty());
}

constexpr s32 kLowest = std::numeric_limits<s32>::min();

TEST_CASE("every boss code keeps its legend staging", "[game][bosses][legend-show][boss-kinds]") {
    using Flight = LegendShow::Flight;
    struct Row {
        s32 code;
        bool inHand;
        PlayerDeed gesture;
        Flight flight;
        std::string_view resting;
        std::string_view burst;
        f32 burstSeconds;
        Vec3 offset;
        std::string_view trail;
    };
    constexpr PlayerDeed kHurl = PlayerDeed::HurlLegend;
    constexpr PlayerDeed kThrow = PlayerDeed::ThrowLegend;
    constexpr PlayerDeed kShoot = PlayerDeed::ShootLegend;
    constexpr std::array<Row, 15> kRows{{
        {-1, false, kHurl, Flight::AtBoss, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, ""},
        {33, false, kHurl, Flight::AtBoss, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, ""},
        {34, true, kThrow, Flight::Flies, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, "PARTICLE1_A"},
        {35, true, kThrow, Flight::Flies, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, "CHIMKEY_PART"},
        {36, true, kShoot, Flight::Flies, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, ""},
        {37, true, kShoot, Flight::WithBearer, "LEGENDPRJ", "LEGENDFX", 3, Vec3{}, ""},
        {38, true, kThrow, Flight::Flies, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, "PARTICLE1_A"},
        {39, true, kThrow, Flight::AtBoss, "LEGENDFX", "LEGENDFX2", 30, Vec3{-7, -5, 85}, ""},
        {40, false, kHurl, Flight::AtBoss, "LEGENDPRJ", "LEGENDFX", 30, Vec3{0, -5, 10}, ""},
        {41, false, kHurl, Flight::AtBoss, "LEGENDPRJ", "LEGENDFX", 5, Vec3{}, ""},
        {42, false, kHurl, Flight::AtBoss, "LEGENDPRJ", "LEGENDFX", 30, Vec3{0, 17, 3}, ""},
        {43, false, kHurl, Flight::AtBoss, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, ""},
        {44, false, kHurl, Flight::AtBoss, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, ""},
        {45, false, kHurl, Flight::AtBoss, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, ""},
        {kLowest, false, kHurl, Flight::AtBoss, "LEGENDPRJ", "LEGENDFX", 30, Vec3{}, ""},
    }};
    ParticleDescriptor trailed = LegendShow::trailOf(34);
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        CHECK(LegendShow::heldInHand(row.code) == row.inHand);
        CHECK(LegendShow::gestureOf(row.code) == row.gesture);
        CHECK(LegendShow::flightOf(row.code) == row.flight);
        CHECK(LegendShow::restingTreeOf(row.code) == row.resting);
        CHECK(LegendShow::burstTreeOf(row.code) == row.burst);
        CHECK(LegendShow::burstSecondsOf(row.code) == row.burstSeconds);
        CHECK(LegendShow::bossOffsetOf(row.code) == row.offset);
        // A trail differs from the dragon's only by its texture; no trail is an empty one.
        const ParticleDescriptor trail = LegendShow::trailOf(row.code);
        CHECK(trail.texture == row.trail);
        if (row.trail.empty()) {
            CHECK(trail == ParticleDescriptor{});
        } else {
            trailed.texture = row.trail;
            CHECK(trail == trailed);
        }
    }
}

TEST_CASE("each weakened boss keeps its roar wait and darkness and the way its rite ends",
          "[game][bosses][legend-show][boss-kinds]") {
    struct Row {
        s32 code;
        s32 roarWait;       ///< ticks after rising
        bool darkAfterRoar; ///< the presentation stays dark once the roar is over
        bool overAtRoar;    ///< nothing wears off: the rite ends with the roar
        bool endsOnImpact;  ///< the item's landing ends the rite
    };
    constexpr std::array<Row, 9> kRows{{
        {34, 180, true, false, true},
        {35, 60, false, true, false},
        {36, 180, false, false, true},
        {37, 180, false, true, false},
        {38, 180, false, false, false},
        {39, 180, false, false, false},
        {40, 180, false, false, false},
        {41, 60, false, true, false},
        {42, 60, false, false, false},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        const LegendWeakness* weakness = legendWeaknessOf(row.code);
        REQUIRE(weakness != nullptr);
        LegendRite rite;
        rite.begin(0, *weakness);
        rite.update(1, true, false);
        CHECK(rite.darkens());
        s32 ticks = 1;
        while (!rite.wantsRoar() && ticks < 1000) {
            rite.update(1, true, false);
            ++ticks;
        }
        CHECK(ticks == row.roarWait);
        CHECK(rite.thrown());
        CHECK(rite.darkens());
        const auto cues = rite.update(1, true, true);
        REQUIRE_FALSE(cues.empty());
        CHECK(cues.back() == LegendCue::Roared);
        CHECK(rite.darkens() == row.darkAfterRoar);
        CHECK(rite.running() == !row.overAtRoar);
        CHECK(rite.finishOnImpact() == row.endsOnImpact);
        if (row.endsOnImpact) {
            CHECK_FALSE(rite.running());
            CHECK_FALSE(rite.darkens());
        }
    }
}

TEST_CASE("each boss code's legend weakness keeps its native digest bytes",
          "[game][bosses][legend-show][native-state-digest][boss-kinds]") {
    // SHA-256 of the weakness's native digest, which the rite carries into netplay state;
    // empty for a code no item touches.
    struct Row {
        s32 code;
        std::string_view hash;
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, ""},
        {33, ""},
        {34, "b6c2928d257cebe53af4639d60401b8b8b41edcdf59dfc12be1ece237b573781"},
        {35, "51ee539d7ee1fa3f54092ca342e34e09d054298ddaa3f789e9f220f6013f2aaf"},
        {36, "21ec8e81b5280d2aad41e64c283255a5535560260a2a0ce0a969dd26e4fa0764"},
        {37, "c1f7330fff4839cf8b8eb4cc800ea8712730ffcf7c6e8db592140d87aa27d464"},
        {38, "4ac2a580bf031b99c9d6599f9247d86d30cf518c78d098a0c7b01696fcccb679"},
        {39, "463ff2ec99f5d8766c67e8570ff5ba827c60c4d7f8f0d19f29acce478304a761"},
        {40, "52d1ad9cd4656b6759c38a6e5529dc288cca3b51191ebba7ade9b96b124b52f2"},
        {41, "9c2e42dd6f134e10eb3eadfc180389afaf7de86432e0cc4d34bb6058792b8415"},
        {42, "db5afc66edaa019cc52dadac3a60c295eb381d696ad57896a29884d59e284b11"},
        {43, ""},
        {44, ""},
        {45, ""},
        {kLowest, ""},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        const LegendWeakness* weakness = legendWeaknessOf(row.code);
        REQUIRE((weakness != nullptr) == !row.hash.empty());
        if (weakness != nullptr) {
            NativeStateDigest digest;
            digest.value(*weakness);
            CHECK(test::hexBytes(digest.finish()) == row.hash);
        }
    }
}

TEST_CASE("every boss code keeps when its legend strikes and how its rite roars and darkens",
          "[game][bosses][legend-show][boss-kinds]") {
    using Strike = LegendStrike;
    struct Row {
        s32 code;
        Strike strike;
        bool whileRiteRuns;
        bool onRelease;
        bool quickToRoar;
        bool darkUntilImpact;
        bool restoresSight;
        bool homes;
    };
    constexpr std::array<Row, 15> kRows{{
        {-1, Strike::OnThrow, false, false, false, false, false, false},
        {33, Strike::OnThrow, false, false, false, false, false, false},
        {34, Strike::AtRiteEnd, false, false, false, true, false, false},
        {35, Strike::OnLanding, false, false, true, false, false, false},
        {36, Strike::AtRiteEnd, false, false, false, false, false, false},
        {37, Strike::OnLanding, false, true, false, false, false, false},
        {38, Strike::OnLanding, true, false, false, false, true, true},
        {39, Strike::OnThrow, false, false, false, false, false, false},
        {40, Strike::OnThrow, false, false, false, false, false, false},
        {41, Strike::OnThrow, false, false, true, false, false, false},
        {42, Strike::OnLanding, false, true, true, false, false, false},
        {43, Strike::OnThrow, false, false, false, false, false, false},
        {44, Strike::OnThrow, false, false, false, false, false, false},
        {45, Strike::OnThrow, false, false, false, false, false, false},
        {kLowest, Strike::OnThrow, false, false, false, false, false, false},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        CHECK(LegendShow::strikeOf(row.code) == row.strike);
        CHECK(LegendShow::landsWhileRiteRuns(row.code) == row.whileRiteRuns);
        CHECK(LegendShow::landsOnRelease(row.code) == row.onRelease);
        CHECK(LegendShow::quickToRoar(row.code) == row.quickToRoar);
        CHECK(LegendShow::darkUntilImpact(row.code) == row.darkUntilImpact);
        CHECK(LegendShow::restoresSight(row.code) == row.restoresSight);
        CHECK(LegendShow::homes(row.code) == row.homes);
    }
}

TEST_CASE("every boss code keeps what its legend strike leaves on it",
          "[game][bosses][legend-show][boss-kinds]") {
    struct Row {
        s32 code;
        std::string_view eye;
        std::optional<Color> tint;
        std::string_view stump;
        std::string_view node;
        std::string_view skin;
        f32 followSeconds;
    };
    constexpr std::optional<Color> kPoisoned = Color::rgba(64, 255, 64);
    constexpr std::array<Row, 15> kRows{{
        {-1, "", {}, "", "", "", 0},
        {33, "", {}, "", "", "", 0},
        {34, "", {}, "", "", "SEETHROUGH", 0},
        {35, "", {}, "STUMPLQ", "CHIMLIONHEA", "", 0},
        {36, "", {}, "", "", "", 28},
        {37, "", kPoisoned, "", "", "", 0},
        {38, "PBOSSQEYEBALL", {}, "", "BODY1_EYEBALL", "", 0},
        {39, "", {}, "", "", "", 0},
        {40, "", {}, "", "", "", 0},
        {41, "", {}, "", "", "", 0},
        {42, "", {}, "", "", "", 0},
        {43, "", {}, "", "", "", 0},
        {44, "", {}, "", "", "", 0},
        {45, "", {}, "", "", "", 0},
        {kLowest, "", {}, "", "", "", 0},
    }};
    for (const Row& row : kRows) {
        CAPTURE(row.code);
        CHECK(LegendShow::eyeReplacementOf(row.code) == row.eye);
        CHECK(LegendShow::strikeTintOf(row.code) == row.tint);
        CHECK(LegendShow::lionStumpOf(row.code) == row.stump);
        CHECK(LegendShow::targetNodeOf(row.code) == row.node);
        CHECK(LegendShow::frozenSkinOf(row.code) == row.skin);
        CHECK(LegendShow::burstFollowSecondsOf(row.code) == row.followSeconds);
    }
}

} // namespace
