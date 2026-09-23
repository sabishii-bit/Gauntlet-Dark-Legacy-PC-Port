#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/enemies/LegendItems.h"

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
    REQUIRE(spider->curbLasts == 0.0f);
    REQUIRE(spider->scale == Approx(0.8f));
    REQUIRE(legendWeaknessOf(39)->curbLasts == Approx(29.0f));
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
    for (int i = 0; i < 100; ++i) {
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
    int ticks = 2;
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
    int worn = 0;
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
    for (int i = 0; i < 200; ++i) {
        forGood.update(2, true, false);
    }
    cues = forGood.update(2, true, true);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0] == LegendCue::Roared);
    REQUIRE(forGood.stage() == LegendRite::Stage::Over);
    forGood.clear();
    REQUIRE(forGood.stage() == LegendRite::Stage::None);
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
    REQUIRE(LegendShow::bossOffsetOf(40).z == Approx(2.5625f));
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

} // namespace
