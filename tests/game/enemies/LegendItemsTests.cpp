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
    // Risen: the item goes up at once, and is thrown a second on.
    auto cues = rite.update(2, true, false);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0] == LegendCue::Brandished);
    REQUIRE(rite.stage() == LegendRite::Stage::Woken);
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
    // Its roar done, the curb runs twenty-nine seconds and wears off.
    cues = rite.update(2, true, true);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0] == LegendCue::Roared);
    REQUIRE_FALSE(rite.wantsRoar());
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

} // namespace
