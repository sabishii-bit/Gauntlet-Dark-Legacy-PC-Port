#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/hud/PartyHud.h"
#include "game/party/PickupPresentation.h"
#include "game/party/RecordingPickupPresentation.h"
#include "game/players/ItemPickup.h"
#include "game/players/PlayerActor.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<PickupPresentation>);
static_assert(std::has_virtual_destructor_v<PickupPresentation>);

TEST_CASE("a recording pickup presentation writes every call down in turn",
          "[game][pickups][pickup-presentation]") {
    test::RecordingPickupPresentation recorded;
    PickupPresentation& presentation = recorded;
    PlayerActor actor;
    actor.spawn(2, {}, nullptr, {0, 0, 0}, 0);
    presentation.addCard(1, "CRYSTAL");
    presentation.showCount(3, "SM_FANGS", 5, 12);
    presentation.focusPickup(actor, 6, 4);
    presentation.showRelics();
    CHECK(recorded.log == std::vector<std::string>{"card 1 CRYSTAL", "count 3 SM_FANGS 5/12",
                                                   "focus 2 6 4", "relics"});
}

TEST_CASE("a recording pickup presentation passes every call on to the HUD it is given",
          "[game][pickups][pickup-presentation]") {
    PartyHud hud;
    test::RecordingPickupPresentation recorded{hud};
    PickupPresentation& presentation = recorded;
    PlayerActor actor;
    actor.spawn(3, {}, nullptr, {0, 0, 0}, 0);
    constexpr auto kSpeed = static_cast<s32>(ItemKind::SpeedPowerup);
    actor.save().progress().inventory.addPowerup(kSpeed, 0, 20, 15, false);
    REQUIRE(hud.selector(3).selection() == -1);
    REQUIRE_FALSE(hud.relicsShown());

    presentation.addCard(3, "SPECIALS");
    presentation.showCount(1, "SM_CLAWS", 4, 28);
    presentation.focusPickup(actor, kSpeed, 0);
    presentation.showRelics();

    REQUIRE(hud.pickups().cards().size() == 1);
    CHECK(hud.pickups().cards()[0].player == 3);
    CHECK(hud.pickups().cards()[0].texture == "SPECIALS");
    REQUIRE_FALSE(hud.pickups().counts(1).empty());
    const auto& count = hud.pickups().counts(1)[0];
    CHECK(count.showing());
    CHECK(count.icon == "SM_CLAWS");
    CHECK(count.count == 4);
    CHECK(count.total == 28);
    CHECK(hud.selector(3).selection() == 0);
    CHECK(hud.relicsShown());
    CHECK(recorded.log.size() == 4);
}
} // namespace
