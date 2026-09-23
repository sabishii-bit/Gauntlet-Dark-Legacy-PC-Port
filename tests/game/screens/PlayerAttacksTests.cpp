#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "game/screens/PlayerAttacks.h"
namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    PlayerAttacks attacks;
    std::array<PlayerRuntime, 1> players;
    PlayerAttacks::Targets targets{opponents, fixtures, {}};
    Fixture() {
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    }
};

TEST_CASE("player shields consume one potion and expire even without artwork",
          "[game][screens][player-attacks]") {
    Fixture f;
    auto& inventory = f.players[0].actor.save().progress().inventory;
    inventory.addPotions(2, 2);
    f.attacks.shieldPotion(5, f.players);
    REQUIRE(inventory.potions.size() == 2);
    f.attacks.shieldPotion(0, f.players);
    REQUIRE(inventory.potions.size() == 1);
    REQUIRE(f.attacks.shieldCount() == 1);
    f.attacks.updateShields(2, f.players, f.targets);
    REQUIRE(f.attacks.shieldCount() == 1);
    f.attacks.updateShields(1, f.players, f.targets);
    REQUIRE(f.attacks.shieldCount() == 0);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("player shields stop following participants who have fallen or left",
          "[game][screens][player-attacks]") {
    Fixture f;
    f.players[0].actor.save().progress().inventory.addPotions(1, 2);
    f.attacks.shieldPotion(0, f.players);
    f.players[0].life = PlayerLife::Dying;
    f.attacks.updateShields(0.1f, f.players, f.targets);
    REQUIRE(f.attacks.shieldCount() == 0);
    f.players[0].life = PlayerLife::Standing;
    f.attacks.shieldPotion(0, f.players);
    f.attacks.updateShields(0.1f, {}, f.targets);
    REQUIRE(f.attacks.shieldCount() == 0);
}

TEST_CASE("block presentation limits duration and cannot restart during its cooldown",
          "[game][screens][player-attacks]") {
    Fixture f;
    f.attacks.showBlock(0, 2, 100, f.players);
    REQUIRE(f.players[0].blockLeft == 0);
    f.attacks.showBlock(0, 3, 1, f.players);
    REQUIRE(f.players[0].blockLeft == Approx(0.333f));
    f.attacks.showBlock(0, 3, 1000, f.players);
    REQUIRE(f.players[0].blockLeft == Approx(0.333f));
    f.players[0].blockLeft = 0;
    f.attacks.showBlock(0, 3, 1000, f.players);
    REQUIRE(f.players[0].blockLeft == 1);
}

TEST_CASE("player attacks clear transient state and safely ignore closed or missing figures",
          "[game][screens][player-attacks]") {
    Fixture f;
    f.players[0].actor.save().progress().inventory.addPotions(1, 2);
    f.attacks.shieldPotion(0, f.players);
    f.attacks.updateTurbo(0, 2, 0.1f, f.players,
                          [](s32, usize) { FAIL("No figure, no turbo announcement"); });
    REQUIRE(f.attacks.strikes().count() == 0);
    f.attacks.clear();
    f.attacks.clear();
    f.attacks.shieldPotion(0, f.players);
    REQUIRE(f.attacks.shieldCount() == 0);
    REQUIRE(f.players[0].actor.save().progress().inventory.potions.size() == 1);
}
} // namespace
