#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "game/tower/GatewayDeparture.h"
#include "game/tower/TowerGateways.h"

namespace {
using namespace gdl;
using namespace gdl::game;
} // namespace

TEST_CASE("a party bound through the gateways sparkles once, then leaves 35 ticks on",
          "[game][tower][gateways]") {
    // An update is two ticks at 30 updates a second and one at 60.
    const s32 ticks = GENERATE(1, 2);
    CAPTURE(ticks);
    GatewayDeparture departure;
    CHECK_FALSE(departure.active());
    CHECK_FALSE(departure.holding());
    departure.begin(GatewayDestination{1});
    // Chosen, the party waits for Sumner's scroll to go, held, its place kept.
    CHECK(departure.stage() == GatewayDeparture::Stage::Asked);
    CHECK(departure.holding());
    CHECK(departure.destination() == GatewayDestination{1});
    CHECK_FALSE(departure.departed());
    // The first update free of the scroll puts the teleport on the party.
    const GatewayDeparture::Step first = departure.update(ticks);
    CHECK(first.sparkle);
    CHECK_FALSE(first.leave);
    CHECK(departure.stage() == GatewayDeparture::Stage::Sparkling);
    s32 sparkles = 1;
    s32 elapsed = 0;
    bool left = false;
    for (s32 update = 0; update < 100 && !left; ++update) {
        const GatewayDeparture::Step step = departure.update(ticks);
        elapsed += ticks;
        sparkles += step.sparkle ? 1 : 0;
        left = step.leave;
        if (!left) {
            CHECK(departure.holding());
            CHECK_FALSE(departure.departed());
        }
    }
    REQUIRE(left);
    CHECK(sparkles == 1);
    // DoGoodWizard's 35 ticks (0x80343B60), to the update that ends them at either rate.
    CHECK(elapsed >= GatewayDeparture::kSparkleTicks);
    CHECK(elapsed < GatewayDeparture::kSparkleTicks + ticks);
    CHECK(departure.stage() == GatewayDeparture::Stage::Gone);
    CHECK_FALSE(departure.holding());
    CHECK(departure.departed() == GatewayDestination{1});
    CHECK(departure.destination() == GatewayDestination{1});
    // Gone, it brings about nothing more.
    const GatewayDeparture::Step after = departure.update(ticks);
    CHECK_FALSE(after.sparkle);
    CHECK_FALSE(after.leave);
    CHECK(departure.departed() == GatewayDestination{1});
}

TEST_CASE("a journey through the gateways called off leaves the party where it stood",
          "[game][tower][gateways]") {
    const GatewayDestination home;
    for (const s32 updates : {0, 1, 40}) {
        CAPTURE(updates);
        GatewayDeparture departure;
        departure.begin(home);
        for (s32 i = 0; i < updates; ++i) {
            departure.update(1);
        }
        REQUIRE(departure.active());
        CHECK(departure.departed().has_value() == (updates == 40));
        departure.cancel();
        CHECK(departure.stage() == GatewayDeparture::Stage::None);
        CHECK_FALSE(departure.active());
        CHECK_FALSE(departure.holding());
        CHECK_FALSE(departure.destination());
        CHECK_FALSE(departure.departed());
        const GatewayDeparture::Step step = departure.update(1);
        CHECK_FALSE(step.sparkle);
        CHECK_FALSE(step.leave);
    }
}
