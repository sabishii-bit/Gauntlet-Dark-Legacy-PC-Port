#include <array>

#include <catch2/catch_test_macros.hpp>

#include "game/screens/PartyHud.h"
namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("party HUD status uses player identity rather than party order",
          "[game][screens][party-hud]") {
    std::array<PlayerRuntime, 2> players;
    CharacterSave save;
    save.name = "THREE";
    save.gold = 42;
    save.progress().health = 700;
    save.progress().inventory.addKeys(2);
    save.progress().inventory.addPotions(3, 2);
    players[0].actor.spawn(3, save, nullptr, Vec3{0}, 0);
    save.name = "ONE";
    players[1].actor.spawn(1, save, nullptr, Vec3{0}, 0);
    const auto status = PartyHud::status(3, players);
    REQUIRE(status.active);
    REQUIRE(status.name == "THREE");
    REQUIRE(status.health == 700);
    REQUIRE(status.keys == 2);
    REQUIRE(status.potions == 2);
    REQUIRE(status.potionKind == 3);
    REQUIRE(status.gold == 42);
    REQUIRE(status.turbo.has_value());
    REQUIRE(PartyHud::status(1, players).name == "ONE");
    REQUIRE_FALSE(PartyHud::status(0, players).active);
    REQUIRE_FALSE(PartyHud::status(-1, players).active);
}

TEST_CASE("party HUD hides carried goods and health while a character falls",
          "[game][screens][party-hud]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, {}, nullptr, Vec3{0}, 0);
    players[0].actor.save().progress().health = 1;
    players[0].actor.save().progress().inventory.addKeys(5);
    players[0].life = PlayerLife::Dying;
    const auto dying = PartyHud::status(2, players);
    REQUIRE(dying.active);
    REQUIRE(dying.health == 0);
    REQUIRE(dying.keys == 0);
    REQUIRE_FALSE(dying.inTower);
    REQUIRE_FALSE(dying.turbo.has_value());
    players[0].life = PlayerLife::InTower;
    REQUIRE(PartyHud::status(2, players).inTower);
}

TEST_CASE("party HUD clears transient presentation without altering participants",
          "[game][screens][party-hud]") {
    PartyHud hud;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    players[0].actor.save().progress().health = 123;
    REQUIRE_FALSE(hud.postHelp(0, 9, players, audio));
    hud.pickups().addCard(3, "GOLD");
    hud.clear();
    hud.clear();
    REQUIRE_FALSE(hud.help().showing());
    REQUIRE_FALSE(hud.selector(3).showing());
    REQUIRE(players[0].actor.save().health() == 123);
}
} // namespace
