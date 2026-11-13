#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeGameServices.h"
#include "game/tower/TowerGateways.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<TowerGateways>);
static_assert(std::has_virtual_destructor_v<TowerGateways>);

TEST_CASE("the worlds past Sumner's gateways come as offered, every ask a look of its own",
          "[game][tower][gateways]") {
    test::FixedGateways fixed;
    TowerGateways& gateways = fixed;
    CHECK(gateways.worlds().empty()); // an installation with nothing past them
    CHECK(fixed.asks == 1);
    fixed.offered = {{3, "a", true}, {1, "b", false}};
    const auto worlds = gateways.worlds();
    CHECK(fixed.asks == 2);
    // In the order offered, ids and names as their expansions give them, usable or not.
    REQUIRE(worlds.size() == 2);
    CHECK(worlds[0] == GatewayWorld{3, "a", true});
    CHECK(worlds[1] == GatewayWorld{1, "b", false});
    CHECK(worlds[1] != GatewayWorld{1, "b", true});
    CHECK(GatewayWorld{} == GatewayWorld{0, "", false}); // none usable until found
}
} // namespace
