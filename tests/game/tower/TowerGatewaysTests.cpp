#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "FakeGameServices.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Relics.h"
#include "game/level/LevelCatalog.h"
#include "game/level/TowerAccess.h"
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

TEST_CASE("a character has beaten Garm while the class it plays holds his piece",
          "[game][tower][gateways]") {
    // His piece is the battlefield's, eleventh in the tower's order (0x80124D14).
    REQUIRE(LevelRef::orderOf(TowerAccess::kBattlefieldWorld) == 11);
    CharacterSave save;
    CHECK_FALSE(beatenGarm(save));
    // Every other boss's piece, the ten a Sumner starts with, is not his.
    save.progress().relics.shards = Relics::kAllShards;
    CHECK_FALSE(beatenGarm(save));
    save.progress().relics.addShard(11);
    CHECK(beatenGarm(save));
    // Another class keeps its own pieces: playing it, the character has not beaten him.
    save.selectClass(2);
    CHECK_FALSE(beatenGarm(save));
    save.selectClass(0);
    CHECK(beatenGarm(save));
}
} // namespace
