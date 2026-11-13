#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "expansions/shared/app/LegendsTravel.h"
#include "game/characters/Party.h"
#include "game/scenes/GatewayTower.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
} // namespace

TEST_CASE("only the console's goto parks the level a visit to the Legends campaigns leaves",
          "[legends][gateways][legends-travel]") {
    // A first opening starts the visit; the console's alone keeps the level it leaves for goto
    // back, while Sumner's gateway lets Dark Legacy's Tower go, to open it afresh on the way back.
    CHECK(legendsVisitStep(LegendsArrival::Console, false) ==
          LegendsVisitStep{.starts = true, .parks = true});
    CHECK(legendsVisitStep(LegendsArrival::Gateway, false) ==
          LegendsVisitStep{.starts = true, .parks = false});
    // Within a visit nothing starts or parks again: every map left goes, so no Legends world is
    // ever kept as the level to go back to, nor a later map taken for the visit's beginning.
    for (const LegendsArrival arrival : {LegendsArrival::Console, LegendsArrival::Departure,
                                         LegendsArrival::Retreat, LegendsArrival::Gateway}) {
        CAPTURE(static_cast<s32>(arrival));
        CHECK(legendsVisitStep(arrival, true) == LegendsVisitStep{});
    }
}

TEST_CASE("a party come home through Sumner's gateway arrives at the Tower's entrance afresh",
          "[legends][gateways][legends-travel]") {
    const PlayOptions home = gatewayHomeArrival();
    CHECK(home.welcome == false);
    CHECK(home.arriving);
    CHECK(home.arrivalWorld == 0);
    CHECK_FALSE(home.position);
    CHECK_FALSE(home.beaten);
    // The Tower is loaded afresh, so its arrival may still tell a character of the gateways.
    CHECK(home.gatewayScroll);
}

TEST_CASE("Dark Legacy's Tower opens afresh for a party come home through Sumner's gateway",
          "[legends][gateways][legends-travel][assets]") {
    const f64 step = GENERATE(1.0 / 30.0, 1.0 / 60.0);
    CAPTURE(step);
    test::GatewayTower tower;
    // Home from a Legends world: the second member fell there, and the first, past Garm, was
    // never told of the gateways.
    const PartyMember first{0, test::garmBeaten("AB")};
    PartyMember second{1, test::garmBeaten("CD")};
    second.save.gatewaysAnnounced = true;
    second.fallen = true;
    tower.arrive(std::vector{first, second}, step, gatewayHomeArrival());
    // Everyone stands again at the entrance, beside its start marker.
    const WorldLocator* entrance = tower.world.arrivalPoint(0, nullptr);
    REQUIRE(entrance != nullptr);
    for (const s32 player : {0, 1}) {
        CAPTURE(player);
        REQUIRE(tower.scene.actor(player) != nullptr);
        CHECK_FALSE(tower.scene.fallen(player));
        const Vec3 offset = tower.scene.actor(player)->position() - entrance->position;
        CHECK(std::hypot(offset.x, offset.z) <= PlayScene::kSpawnSpacing);
    }
    // No welcome and no level beaten, but Sumner's word of his gateways for the one never told.
    CHECK(tower.scene.intro() == PlayScene::Intro::None);
    CHECK_FALSE(tower.scene.levelBeaten());
    CHECK(tower.tellingGateways());
    tower.scene.close();
}
